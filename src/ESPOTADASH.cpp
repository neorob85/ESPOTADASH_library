#include "ESPOTADASH.h"

#if defined(LIBRETINY)
  #include <Update.h>
#elif defined(ESP32)
  #include <Update.h>
  #include <esp_system.h>
#else
  #include <Updater.h>
#endif

static const unsigned long DEFAULT_REGISTER_INTERVAL = 15UL * 60UL * 1000UL; // 15 minutes

static String formatIp(const IPAddress& ip) {
#if defined(LIBRETINY)
  // LibreTiny's IPAddress has no toString() and wiring_compat.cpp may not be
  // linked; build the dotted-quad manually.
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buf);
#else
  return ip.toString();
#endif
}

static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Extracts the string value of a JSON key from a flat JSON object.
// Only handles simple string values; sufficient for the /cmd body.
static String extractJsonString(const String& json, const String& key) {
  String search = "\"" + key + "\"";
  int ki = json.indexOf(search);
  if (ki < 0) return "";
  int ci = json.indexOf(':', ki + search.length());
  if (ci < 0) return "";
  int qi = json.indexOf('"', ci + 1);
  if (qi < 0) return "";
  int qe = json.indexOf('"', qi + 1);
  if (qe < 0) return "";
  return json.substring(qi + 1, qe);
}

#if !defined(LIBRETINY)
static bool parseJsonByteArray(const String& json, const String& key, uint8_t* buf, uint16_t size) {
  String search = "\"" + key + "\"";
  int ki = json.indexOf(search);
  if (ki < 0) return false;
  int ci = json.indexOf('[', ki + search.length());
  if (ci < 0) return false;
  int ce = json.indexOf(']', ci + 1);
  if (ce < 0) return false;
  String arr = json.substring(ci + 1, ce);
  uint16_t idx = 0;
  int start = 0;
  while (idx < size) {
    int comma = arr.indexOf(',', start);
    String token = (comma < 0) ? arr.substring(start) : arr.substring(start, comma);
    token.trim();
    if (token.length() == 0) break;
    buf[idx++] = (uint8_t)constrain(token.toInt(), 0, 255);
    if (comma < 0) break;
    start = comma + 1;
  }
  return idx == size;
}
#endif

#if defined(ESP32) && !defined(LIBRETINY)
static String getResetReasonString() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External";
    case ESP_RST_SW:        return "Software";
    case ESP_RST_PANIC:     return "Panic";
    case ESP_RST_INT_WDT:   return "Interrupt WDT";
    case ESP_RST_TASK_WDT:  return "Task WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "Unknown";
  }
}
#endif

ESPOTADASH::ESPOTADASH(uint16_t localPort, uint16_t eepromSize, bool littlefs)
  : _localPort(localPort),
    _registerInterval(DEFAULT_REGISTER_INTERVAL),
    _lastRegisterAttempt(0),
    _wasConnected(false),
    _begun(false),
#if !defined(LIBRETINY)
    _eepromSize(eepromSize),
    _littlefsEnabled(littlefs),
    _fsUploadOk(false),
#endif
    _httpServer(localPort) {
#if defined(LIBRETINY)
  (void)eepromSize;
  (void)littlefs;
#endif
}

void ESPOTADASH::addCommand(const String& name, const String& description, std::function<void()> callback) {
  _commands.push_back({ name, description, callback });
}

void ESPOTADASH::begin(const String& serverUrl, const String& deviceName, const String& otaPassword, const String& firmwareVersion) {
  _serverUrl = serverUrl;
  while (_serverUrl.endsWith("/")) _serverUrl.remove(_serverUrl.length() - 1);

  if (deviceName.length()) {
    _deviceName = deviceName;
  } else {
#if defined(LIBRETINY)
    _deviceName = String(LT.getDeviceName());
#elif defined(ESP32)
    _deviceName = String(WiFi.getHostname());
#else
    _deviceName = WiFi.hostname();
#endif
  }

  _firmwareVersion = firmwareVersion;

  _httpServer.on("/",             HTTP_GET,  [this]() { this->handleRoot();        });
  _httpServer.on("/ping",         HTTP_GET,  [this]() { this->handlePing();        });
  _httpServer.on("/info",         HTTP_GET,  [this]() { this->handleInfo();        });
  _httpServer.on("/cmd",          HTTP_POST, [this]() { this->handleCmd();         });
  _httpServer.on("/update",        HTTP_POST,
    [this]() { this->handleUpdateFinish(); },
    [this]() { this->handleUpdateUpload(); });

#if defined(LIBRETINY)
  _httpServer.on("/config",           HTTP_GET,    [this]() { this->handleConfigGet();             });
  _httpServer.on("/config/key",       HTTP_POST,   [this]() { this->handleConfigKeySet();          });
  _httpServer.on("/config/key",       HTTP_DELETE, [this]() { this->handleConfigKeyDelete();       });
  _httpServer.on("/config/namespace", HTTP_DELETE, [this]() { this->handleConfigNamespaceDelete(); });
  _httpServer.on("/config",           HTTP_DELETE, [this]() { this->handleConfigDeleteAll();       });
#else
  _httpServer.on("/eeprom",       HTTP_GET,  [this]() { this->handleEepromGet();   });
  _httpServer.on("/eeprom",       HTTP_POST, [this]() { this->handleEepromWrite(); });
  _httpServer.on("/eeprom/format",HTTP_POST, [this]() { this->handleEepromFormat();});

  if (_littlefsEnabled) {
    LittleFS.begin();
    _httpServer.on("/fs/info",     HTTP_GET,  [this]() { this->handleFsInfo();         });
    _httpServer.on("/fs/list",     HTTP_GET,  [this]() { this->handleFsList();         });
    _httpServer.on("/fs/download", HTTP_GET,  [this]() { this->handleFsDownload();     });
    _httpServer.on("/fs/delete",   HTTP_ANY,  [this]() { this->handleFsDelete();       });
    _httpServer.on("/fs/mkdir",    HTTP_POST, [this]() { this->handleFsMkdir();        });
    _httpServer.on("/fs/upload",   HTTP_POST,
      [this]() { this->handleFsUploadFinish(); },
      [this]() { this->handleFsUploadData();   });
  }
#endif

  _httpServer.begin();

#if !defined(LIBRETINY)
  EEPROM.begin(_eepromSize);

  ArduinoOTA.setHostname(_deviceName.c_str());
  if (otaPassword.length()) ArduinoOTA.setPassword(otaPassword.c_str());
  ArduinoOTA.begin();
#else
  (void)otaPassword;
#endif

  _begun = true;
  _wasConnected = (WiFi.status() == WL_CONNECTED);
  if (_wasConnected) registerNow();
}

void ESPOTADASH::loop() {
  if (!_begun) return;
#if !defined(LIBRETINY)
  ArduinoOTA.handle();
#endif
  _httpServer.handleClient();

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !_wasConnected) {
    registerNow();
  }
  _wasConnected = connected;

  if (connected && (millis() - _lastRegisterAttempt) >= _registerInterval) {
    registerNow();
  }
}

void ESPOTADASH::setRegisterInterval(unsigned long intervalMs) {
  _registerInterval = intervalMs;
}

void ESPOTADASH::setEepromSize(uint16_t size) {
#if !defined(LIBRETINY)
  _eepromSize = size;
#else
  (void)size;
#endif
}

bool ESPOTADASH::registerNow() {
  _lastRegisterAttempt = millis();
  if (WiFi.status() != WL_CONNECTED || _serverUrl.length() == 0) return false;

#if defined(LIBRETINY)
  // HTTPClient is not used on LibreTiny to avoid pulling in setCookie()
  // which calls strptime(), missing from the arm-none-eabi newlib on BK72xx.
  String hostport = _serverUrl;
  if (hostport.startsWith("http://"))  hostport = hostport.substring(7);
  else if (hostport.startsWith("https://")) hostport = hostport.substring(8);

  String host;
  uint16_t port = 80;
  int colon = hostport.indexOf(':');
  if (colon >= 0) {
    host = hostport.substring(0, colon);
    port = (uint16_t)hostport.substring(colon + 1).toInt();
  } else {
    host = hostport;
  }

  String body = buildInfoJson();
  WiFiClient client;
  client.setTimeout(5);
  if (!client.connect(host.c_str(), port)) return false;

  String req;
  req.reserve(128 + body.length());
  req += "POST /api/register HTTP/1.1\r\n";
  req += "Host: " + host + ":" + String(port) + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;
  client.print(req);

  unsigned long deadline = millis() + 5000;
  while (!client.available() && millis() < deadline) delay(10);
  String statusLine = client.readStringUntil('\n');
  client.stop();

  int sp = statusLine.indexOf(' ');
  if (sp < 0) return false;
  int code = statusLine.substring(sp + 1).toInt();
  return (code >= 200 && code < 300);
#else
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  String url = _serverUrl + "/api/register";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(buildInfoJson());
  http.end();
  return (code >= 200 && code < 300);
#endif
}

String ESPOTADASH::buildInfoJson() {
  String s;
  s.reserve(768);
  s += "{";
  s += "\"id\":\""                + jsonEscape(WiFi.macAddress())           + "\",";
  s += "\"name\":\""              + jsonEscape(_deviceName)                 + "\",";
  s += "\"ip\":\""                + formatIp(WiFi.localIP())                + "\",";
  s += "\"mac\":\""               + WiFi.macAddress()                       + "\",";
#if defined(LIBRETINY)
  s += "\"hostname\":\""          + jsonEscape(String(LT.getDeviceName()))  + "\",";
#elif defined(ESP32)
  s += "\"hostname\":\""          + jsonEscape(String(WiFi.getHostname()))  + "\",";
#else
  s += "\"hostname\":\""          + jsonEscape(WiFi.hostname())             + "\",";
#endif
  s += "\"port\":"                + String(_localPort)                      + ",";
#if defined(LIBRETINY)
  s += "\"chipId\":"              + String(ESP.getChipId())                 + ",";
#elif defined(ESP32)
  s += "\"chipId\":"              + String((uint32_t)ESP.getEfuseMac())     + ",";
#else
  s += "\"chipId\":"              + String(ESP.getChipId())                 + ",";
#endif
  s += "\"cpuFreqMHz\":"          + String(ESP.getCpuFreqMHz())             + ",";
  s += "\"freeHeap\":"            + String(ESP.getFreeHeap())               + ",";
#if defined(LIBRETINY)
  s += "\"heapSize\":"            + String(LT.getHeapSize())                + ",";
  s += "\"heapMinFree\":"         + String(LT.getMinFreeHeap())             + ",";
  s += "\"ramSize\":"             + String(LT.getRamSize())                 + ",";
#elif defined(ESP32)
  s += "\"maxFreeBlockSize\":"    + String(ESP.getMaxAllocHeap())           + ",";
#else
  s += "\"heapFragmentation\":"   + String(ESP.getHeapFragmentation())      + ",";
  s += "\"maxFreeBlockSize\":"    + String(ESP.getMaxFreeBlockSize())       + ",";
  s += "\"flashChipRealSize\":"   + String(ESP.getFlashChipRealSize())      + ",";
#endif
  s += "\"flashChipSize\":"       + String(ESP.getFlashChipSize())          + ",";
#if defined(LIBRETINY)
  s += "\"flashChipRealSize\":"   + String(ESP.getFlashChipRealSize())      + ",";
  #if defined(LT_BK72XX)
  {
    // Compute firmware size from linker symbols: all flash content from the app
    // start up to and including the .data section stored in flash.
    extern char _vector_start, _data_flash_begin, _data_ram_begin, _data_ram_end;
    uint32_t sz = (uint32_t)&_data_flash_begin
                + ((uint32_t)&_data_ram_end - (uint32_t)&_data_ram_begin)
                - (uint32_t)&_vector_start;
    s += "\"sketchSize\":"        + String(sz)                              + ",";
  }
  #endif
#else
  s += "\"flashChipSpeed\":"      + String(ESP.getFlashChipSpeed())         + ",";
  s += "\"sketchSize\":"          + String(ESP.getSketchSize())             + ",";
  s += "\"freeSketchSpace\":"     + String(ESP.getFreeSketchSpace())        + ",";
#endif
  s += "\"sdkVersion\":\""        + jsonEscape(String(ESP.getSdkVersion())) + "\",";
#if defined(LIBRETINY)
  s += "\"coreVersion\":\""       + jsonEscape(ESP.getCoreVersion())        + "\",";
  s += "\"resetReason\":\""       + jsonEscape(ESP.getResetReason())        + "\",";
#elif defined(ESP32)
  s += "\"resetReason\":\""       + jsonEscape(getResetReasonString())      + "\",";
#else
  s += "\"coreVersion\":\""       + jsonEscape(ESP.getCoreVersion())        + "\",";
  s += "\"resetReason\":\""       + jsonEscape(ESP.getResetReason())        + "\",";
#endif
  s += "\"rssi\":"                + String(WiFi.RSSI())                     + ",";
  s += "\"ssid\":\""              + jsonEscape(WiFi.SSID())                 + "\",";
  s += "\"uptime\":"              + String(millis())                        + ",";
  if (_firmwareVersion.length())
    s += "\"firmwareVersion\":\"" + jsonEscape(_firmwareVersion)            + "\",";
#if defined(LIBRETINY)
  s += "\"platform\":\"LibreTiny\",";
#elif defined(ESP32)
  s += "\"platform\":\"ESP32\",";
#else
  s += "\"platform\":\"ESP8266\",";
#endif
#if !defined(LIBRETINY)
  s += "\"littlefs\":";
  s += _littlefsEnabled ? "true" : "false";
  s += ",";
  if (_littlefsEnabled) {
  #if defined(ESP32)
    s += "\"lfsTotal\":" + String(LittleFS.totalBytes()) + ",";
    s += "\"lfsUsed\":"  + String(LittleFS.usedBytes())  + ",";
  #else
    FSInfo fs_info;
    if (LittleFS.info(fs_info)) {
      s += "\"lfsTotal\":" + String(fs_info.totalBytes) + ",";
      s += "\"lfsUsed\":"  + String(fs_info.usedBytes)  + ",";
    }
  #endif
  }
#endif
  s += "\"commands\":[";
  for (size_t i = 0; i < _commands.size(); i++) {
    if (i > 0) s += ",";
    s += "{\"name\":\"" + jsonEscape(_commands[i].name) + "\",";
    s += "\"description\":\"" + jsonEscape(_commands[i].description) + "\"}";
  }
  s += "]}";
  return s;
}

void ESPOTADASH::handleRoot() {
  _httpServer.send(200, "text/plain", "ESPOTADASH device: " + _deviceName);
}

void ESPOTADASH::handlePing() {
  String s;
  s.reserve(160);
  s += "{";
  s += "\"ok\":true,";
  s += "\"id\":\""       + WiFi.macAddress()         + "\",";
  s += "\"uptime\":"     + String(millis())          + ",";
  s += "\"freeHeap\":"   + String(ESP.getFreeHeap()) + ",";
  s += "\"rssi\":"       + String(WiFi.RSSI());
  s += "}";
  _httpServer.send(200, "application/json", s);
}

void ESPOTADASH::handleInfo() {
  _httpServer.send(200, "application/json", buildInfoJson());
}

void ESPOTADASH::handleCmd() {
  String body = _httpServer.arg("plain");
  String name = extractJsonString(body, "command");

  if (name.length() == 0) {
    _httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing command\"}");
    return;
  }

  for (auto& cmd : _commands) {
    if (cmd.name == name) {
      _httpServer.send(200, "application/json", "{\"ok\":true,\"command\":\"" + jsonEscape(name) + "\"}");
      cmd.callback();
      return;
    }
  }

  _httpServer.send(404, "application/json", "{\"ok\":false,\"error\":\"unknown command\"}");
}

void ESPOTADASH::handleUpdateUpload() {
  HTTPUpload& upload = _httpServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
#if defined(LIBRETINY)
    Update.begin();
#else
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  #if !defined(ESP32)
    Update.runAsync(true);
  #endif
    if (!Update.begin(maxSketchSpace)) {
      Update.end();
    }
#endif
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void ESPOTADASH::handleUpdateFinish() {
  if (Update.hasError()) {
    _httpServer.send(500, "application/json", "{\"ok\":false,\"error\":\"update failed\"}");
  } else {
    _httpServer.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  }
}

#if !defined(LIBRETINY)

// ---- EEPROM handlers ----

void ESPOTADASH::handleEepromGet() {
  String s;
  s.reserve((size_t)_eepromSize * 4 + 64);
  s += "{\"ok\":true,\"size\":";
  s += String(_eepromSize);
  s += ",\"data\":[";
  for (uint16_t i = 0; i < _eepromSize; i++) {
    if (i > 0) s += ',';
    s += String(EEPROM.read(i));
  }
  s += "]}";
  _httpServer.send(200, "application/json", s);
}

void ESPOTADASH::handleEepromWrite() {
  String body = _httpServer.arg("plain");
  uint8_t* buf = new uint8_t[_eepromSize];
  if (!buf) {
    _httpServer.send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
    return;
  }
  if (!parseJsonByteArray(body, "data", buf, _eepromSize)) {
    delete[] buf;
    _httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid data\"}");
    return;
  }
  for (uint16_t i = 0; i < _eepromSize; i++) {
    EEPROM.write(i, buf[i]);
  }
  EEPROM.commit();
  delete[] buf;
  _httpServer.send(200, "application/json", "{\"ok\":true}");
}

void ESPOTADASH::handleEepromFormat() {
  for (uint16_t i = 0; i < _eepromSize; i++) {
    EEPROM.write(i, 0xFF);
  }
  EEPROM.commit();
  _httpServer.send(200, "application/json", "{\"ok\":true}");
}

// ---- LittleFS handlers ----

void ESPOTADASH::handleFsInfo() {
#if defined(ESP32)
  String s;
  s.reserve(96);
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  s += "{\"ok\":true,\"totalBytes\":";
  s += String(total);
  s += ",\"usedBytes\":";
  s += String(used);
  s += ",\"freeBytes\":";
  s += String(total - used);
  s += "}";
  _httpServer.send(200, "application/json", s);
#else
  FSInfo info;
  if (!LittleFS.info(info)) {
    _httpServer.send(500, "application/json", "{\"ok\":false,\"error\":\"not mounted\"}");
    return;
  }
  String s;
  s.reserve(96);
  s += "{\"ok\":true,\"totalBytes\":";
  s += String(info.totalBytes);
  s += ",\"usedBytes\":";
  s += String(info.usedBytes);
  s += ",\"freeBytes\":";
  s += String(info.totalBytes - info.usedBytes);
  s += "}";
  _httpServer.send(200, "application/json", s);
#endif
}

void ESPOTADASH::handleFsList() {
  String path = _httpServer.arg("path");
  if (!path.length()) path = "/";
  if (!path.startsWith("/")) path = "/" + path;
  while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);

  String s, files;
  s.reserve(512);
  files.reserve(256);
  s += "{\"ok\":true,\"path\":\"";
  s += jsonEscape(path);
  s += "\",\"dirs\":[";

  bool fd = true, ff = true;

#if defined(ESP32)
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) {
    _httpServer.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }
  File entry = dir.openNextFile();
  while (entry) {
    String entryName = entry.name();
    int lastSlash = entryName.lastIndexOf('/');
    if (lastSlash >= 0) entryName = entryName.substring(lastSlash + 1);
    if (entry.isDirectory()) {
      if (!fd) s += ",";
      s += "\""; s += jsonEscape(entryName); s += "\"";
      fd = false;
    } else {
      if (!ff) files += ",";
      files += "{\"name\":\""; files += jsonEscape(entryName);
      files += "\",\"size\":"; files += String(entry.size()); files += "}";
      ff = false;
    }
    entry = dir.openNextFile();
  }
  dir.close();
#else
  Dir dir = LittleFS.openDir(path);
  while (dir.next()) {
    if (dir.isDirectory()) {
      if (!fd) s += ",";
      s += "\""; s += jsonEscape(dir.fileName()); s += "\"";
      fd = false;
    } else {
      if (!ff) files += ",";
      files += "{\"name\":\""; files += jsonEscape(dir.fileName());
      files += "\",\"size\":"; files += String(dir.fileSize()); files += "}";
      ff = false;
    }
  }
#endif

  s += "],\"files\":["; s += files; s += "]}";
  _httpServer.send(200, "application/json", s);
}

void ESPOTADASH::handleFsDownload() {
  String path = _httpServer.arg("path");
  if (!path.startsWith("/")) path = "/" + path;
  if (!LittleFS.exists(path)) {
    _httpServer.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    _httpServer.send(500, "application/json", "{\"ok\":false,\"error\":\"open failed\"}");
    return;
  }
  String name = path.substring(path.lastIndexOf('/') + 1);
  _httpServer.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  _httpServer.streamFile(f, "application/octet-stream");
  f.close();
}

void ESPOTADASH::handleFsDelete() {
  String path = _httpServer.arg("path");
  if (!path.startsWith("/")) path = "/" + path;
  if (!LittleFS.exists(path)) {
    _httpServer.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }
  bool ok = LittleFS.remove(path);
  if (!ok) ok = LittleFS.rmdir(path);
  _httpServer.send(ok ? 200 : 500, "application/json",
    ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete failed\"}");
}

void ESPOTADASH::handleFsMkdir() {
  String path = _httpServer.arg("path");
  if (!path.startsWith("/")) path = "/" + path;
  bool ok = LittleFS.mkdir(path);
  _httpServer.send(ok ? 200 : 500, "application/json",
    ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"mkdir failed\"}");
}

void ESPOTADASH::handleFsUploadData() {
  HTTPUpload& upload = _httpServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String path = _httpServer.arg("path");
    if (!path.startsWith("/")) path = "/" + path;
    _fsUploadPath = path;
    _fsUploadOk = false;
    int sep = path.lastIndexOf('/');
    if (sep > 0) LittleFS.mkdir(path.substring(0, sep));
    _fsUploadFile = LittleFS.open(path, "w");
    _fsUploadOk = (bool)_fsUploadFile;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (_fsUploadFile) _fsUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (_fsUploadFile) _fsUploadFile.close();
  }
}

void ESPOTADASH::handleFsUploadFinish() {
  if (!_fsUploadOk) {
    _httpServer.send(500, "application/json", "{\"ok\":false,\"error\":\"upload failed\"}");
    return;
  }
  _httpServer.send(200, "application/json",
    "{\"ok\":true,\"path\":\"" + jsonEscape(_fsUploadPath) + "\"}");
}

#endif // !LIBRETINY

#if defined(LIBRETINY)

// ---- Config handlers (LibreTiny / PrefsManager) ----

// Reads all keys of a namespace except one, erases the namespace, then rewrites
// the remaining keys. Used to implement per-key deletion without modifying
// PrefsManager, which only exposes namespace-level erase.
static void _prefsDeleteKey(PrefsManager& prefs, const String& ns, const String& key) {
  String keysStr = prefs.listKeysString(ns);
  if (keysStr.length() == 0) return;

  struct KeyData { String name, type, value; };
  KeyData kept[16];
  int count = 0;

  auto parseEntry = [&](const String& entry) {
    int col = entry.indexOf(':');
    String k = col > 0 ? entry.substring(0, col) : entry;
    String t = col > 0 ? entry.substring(col + 1) : "str";
    if (k == key || k.length() == 0 || count >= 16) return;
    kept[count].name = k;
    kept[count].type = t;
    String path = ns + "/" + k;
    char buf64[24];
    if      (t == "int32")  { int32_t  v = 0;     prefs.read(path, v); kept[count].value = String(v); }
    else if (t == "uint32") { uint32_t v = 0;     prefs.read(path, v); kept[count].value = String(v); }
    else if (t == "int64")  { int64_t  v = 0;     prefs.read(path, v); snprintf(buf64, sizeof(buf64), "%lld", (long long)v); kept[count].value = String(buf64); }
    else if (t == "float")  { float    v = 0.0f;  prefs.read(path, v); kept[count].value = String(v, 7); }
    else if (t == "double") { double   v = 0.0;   prefs.read(path, v); kept[count].value = String(v, 15); }
    else if (t == "bool")   { bool     v = false; prefs.read(path, v); kept[count].value = v ? "1" : "0"; }
    else                    { String   v;          prefs.read(path, v); kept[count].value = v; }
    count++;
  };

  int start = 0, sep;
  while ((sep = keysStr.indexOf(',', start)) != -1) {
    parseEntry(keysStr.substring(start, sep));
    start = sep + 1;
  }
  parseEntry(keysStr.substring(start));

  prefs.erase(ns);

  for (int i = 0; i < count; i++) {
    String path = ns + "/" + kept[i].name;
    const String& t = kept[i].type;
    const String& v = kept[i].value;
    if      (t == "int32")  prefs.write(path, (int32_t)v.toInt());
    else if (t == "uint32") prefs.write(path, (uint32_t)strtoul(v.c_str(), nullptr, 10));
    else if (t == "int64")  prefs.write(path, (int64_t)v.toInt());
    else if (t == "float")  prefs.write(path, v.toFloat());
    else if (t == "double") prefs.write(path, v.toDouble());
    else if (t == "bool")   prefs.write(path, v == "1");
    else                    prefs.write(path, v);
  }
}

void ESPOTADASH::handleConfigGet() {
  _httpServer.send(200, "application/json", _prefs.listAllJson());
}

void ESPOTADASH::handleConfigKeySet() {
  String ns   = _httpServer.arg("ns");
  String key  = _httpServer.arg("key");
  String body = _httpServer.arg("plain");
  if (ns.length() == 0 || key.length() == 0) {
    _httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ns or key\"}");
    return;
  }
  String type  = extractJsonString(body, "type");
  String value = extractJsonString(body, "value");
  String path  = ns + "/" + key;
  if      (type == "int32")  _prefs.write(path, (int32_t)value.toInt());
  else if (type == "uint32") _prefs.write(path, (uint32_t)strtoul(value.c_str(), nullptr, 10));
  else if (type == "int64")  _prefs.write(path, (int64_t)value.toInt());
  else if (type == "float")  _prefs.write(path, value.toFloat());
  else if (type == "double") _prefs.write(path, value.toDouble());
  else if (type == "bool")   _prefs.write(path, value == "true");
  else                       _prefs.write(path, value);
  _httpServer.send(200, "application/json", "{\"ok\":true}");
}

void ESPOTADASH::handleConfigKeyDelete() {
  String ns  = _httpServer.arg("ns");
  String key = _httpServer.arg("key");
  if (ns.length() == 0 || key.length() == 0) {
    _httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ns or key\"}");
    return;
  }
  _prefsDeleteKey(_prefs, ns, key);
  _httpServer.send(200, "application/json", "{\"ok\":true}");
}

void ESPOTADASH::handleConfigNamespaceDelete() {
  String ns = _httpServer.arg("ns");
  if (ns.length() == 0) {
    _httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ns\"}");
    return;
  }
  _prefs.erase(ns);
  _httpServer.send(200, "application/json", "{\"ok\":true}");
}

void ESPOTADASH::handleConfigDeleteAll() {
  _prefs.eraseAll();
  _httpServer.send(200, "application/json", "{\"ok\":true}");
}

#endif // LIBRETINY
