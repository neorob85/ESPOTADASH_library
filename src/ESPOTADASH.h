#ifndef ESPOTADASH_H
#define ESPOTADASH_H

#include <Arduino.h>
#include <functional>
#include <vector>

#if defined(LIBRETINY)
  #include <WiFi.h>
  #include <WebServer.h>
  using ESPOTADASH_WebServer = WebServer;
#elif defined(ESP32)
  #include <ArduinoOTA.h>
  #include <EEPROM.h>
  #include <LittleFS.h>
  #include <WiFi.h>
  #include <WebServer.h>
  #include <HTTPClient.h>
  using ESPOTADASH_WebServer = WebServer;
#else
  #include <ArduinoOTA.h>
  #include <EEPROM.h>
  #include <LittleFS.h>
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266HTTPClient.h>
  using ESPOTADASH_WebServer = ESP8266WebServer;
#endif

class ESPOTADASH {
public:
  struct Command {
    String name;
    String description;
    std::function<void()> callback;
  };

  // On LibreTiny, eepromSize and littlefs are accepted but ignored
  // (EEPROM and LittleFS are not supported on this platform).
  explicit ESPOTADASH(uint16_t localPort = 80, uint16_t eepromSize = 512, bool littlefs = false);

  void begin(const String& serverUrl, const String& deviceName = "", const String& otaPassword = "", const String& firmwareVersion = "");
  void loop();

  void setRegisterInterval(unsigned long intervalMs);
  void setEepromSize(uint16_t size);
  bool registerNow();
  void addCommand(const String& name, const String& description, std::function<void()> callback);

  String buildInfoJson();

private:
  void handleRoot();
  void handlePing();
  void handleInfo();
  void handleCmd();
  void handleUpdateFinish();
  void handleUpdateUpload();

#if !defined(LIBRETINY)
  void handleEepromGet();
  void handleEepromWrite();
  void handleEepromFormat();
  void handleFsInfo();
  void handleFsList();
  void handleFsDownload();
  void handleFsDelete();
  void handleFsMkdir();
  void handleFsUploadFinish();
  void handleFsUploadData();
#endif

  String _serverUrl;
  String _deviceName;
  String _firmwareVersion;
  uint16_t _localPort;
  unsigned long _registerInterval;
  unsigned long _lastRegisterAttempt;
  bool _wasConnected;
  bool _begun;
#if !defined(LIBRETINY)
  uint16_t _eepromSize;
  bool _littlefsEnabled;
  bool _fsUploadOk;
  String _fsUploadPath;
  fs::File _fsUploadFile;
#endif
  ESPOTADASH_WebServer _httpServer;
  std::vector<Command> _commands;
};

#endif
