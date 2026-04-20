# ESPOTADASH — API Reference

ESPOTADASH is an Arduino library for ESP8266 and ESP32 that automatically registers a device with a Node.js dashboard server. It exposes a local HTTP interface for monitoring, remote commands, OTA firmware updates, and LittleFS filesystem management.

You need first install the server at https://github.com/neorob85/ESPOTADASH_server.git

---

## Table of Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Basic Usage](#basic-usage)
- [Constructor](#constructor)
- [Public Functions](#public-functions)
  - [begin()](#begin)
  - [loop()](#loop)
  - [addCommand()](#addcommand)
  - [registerNow()](#registernow)
  - [setRegisterInterval()](#setregisterinterval)
  - [setEepromSize()](#seteepromsize)
  - [buildInfoJson()](#buildinfojson)
- [HTTP Endpoints](#http-endpoints)
- [ESP8266 / ESP32 Compatibility](#esp8266--esp32-compatibility)

---

## Requirements

| Platform | Framework | Dependencies                                                     |
|----------|-----------|------------------------------------------------------------------|
| ESP8266  | Arduino   | `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266HTTPClient`, `ArduinoOTA`, `EEPROM`, `LittleFS` |
| ESP32    | Arduino   | `WiFi`, `WebServer`, `HTTPClient`, `ArduinoOTA`, `EEPROM`, `LittleFS` |

---

## Installation

### PlatformIO
Add the library to your `platformio.ini`:

```ini
lib_deps =
  neorob85/ESPOTADASH_library
```

### Arduino IDE
Copy the `ESPOTADASH` folder into the Arduino `libraries` directory.

---

## Basic Usage

```cpp
#include <ESP8266WiFi.h>   // or <WiFi.h> on ESP32
#include <ESPOTADASH.h>

ESPOTADASH dash(80, 512, false);

void setup() {
  WiFi.begin("ssid", "password");
  while (WiFi.status() != WL_CONNECTED) delay(250);

  dash.addCommand("reboot", "Restart the device", []() {
    ESP.restart();
  });

  dash.begin("http://192.168.1.100:3000", "my-esp", "otapass", "1.0.0");
}

void loop() {
  dash.loop();
}
```

---

## Constructor

```cpp
ESPOTADASH(uint16_t localPort = 80, uint16_t eepromSize = 512, bool littlefs = false)
```

| Parameter    | Type       | Default | Description                                                      |
|--------------|------------|---------|------------------------------------------------------------------|
| `localPort`  | `uint16_t` | `80`    | TCP port for the device's local HTTP server.                     |
| `eepromSize` | `uint16_t` | `512`   | EEPROM region size in bytes to initialise.                       |
| `littlefs`   | `bool`     | `false` | If `true`, mounts LittleFS and enables the `/fs/*` endpoints.   |

**Example:**
```cpp
// HTTP on port 8080, 1024-byte EEPROM, LittleFS enabled
ESPOTADASH dash(8080, 1024, true);
```

---

## Public Functions

### `begin()`

```cpp
void begin(const String& serverUrl,
           const String& deviceName      = "",
           const String& otaPassword     = "",
           const String& firmwareVersion = "")
```

Initialises the library. Must be called in `setup()`, **after** a WiFi connection has been established.

| Parameter         | Description                                                                                      |
|-------------------|--------------------------------------------------------------------------------------------------|
| `serverUrl`       | Base URL of the ESPOTADASH server (e.g. `"http://192.168.1.100:3000"`). Trailing slashes are stripped automatically. |
| `deviceName`      | Device name sent to the dashboard. If empty, the current WiFi hostname is used.                  |
| `otaPassword`     | Password for Arduino OTA updates. If empty, OTA requires no password.                            |
| `firmwareVersion` | Firmware version string (e.g. `"1.2.3"`). If empty, the field is omitted from the registration payload. |

Internally this function:
1. Registers all HTTP endpoints on the local server.
2. Initialises EEPROM and ArduinoOTA.
3. Mounts LittleFS (if enabled in the constructor).
4. Starts the HTTP server.
5. Performs the first registration with the dashboard server.

---

### `loop()`

```cpp
void loop()
```

Handles polling for the local HTTP server, ArduinoOTA, and periodic re-registration with the dashboard. **Must be called on every iteration of `loop()`.**

- If the device loses and regains a WiFi connection, it re-registers automatically.
- Periodic re-registration runs at the configured interval (default every 15 minutes).

---

### `addCommand()`

```cpp
void addCommand(const String& name,
                const String& description,
                std::function<void()> callback)
```

Registers a command that the dashboard server can invoke via `POST /cmd`.

| Parameter     | Description                                                                                  |
|---------------|----------------------------------------------------------------------------------------------|
| `name`        | Unique command identifier (e.g. `"reboot"`). Case-sensitive.                                 |
| `description` | Human-readable label shown as a button in the dashboard command panel.                        |
| `callback`    | A `void()` lambda or function pointer executed when the command is received.                 |

> **Note:** `addCommand()` must be called **before** `begin()`, otherwise the commands will not be included in the registration payload.

**Example:**
```cpp
dash.addCommand("toggle_led", "Toggle the built-in LED", []() {
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
});

dash.addCommand("heap_report", "Print free heap to Serial", []() {
  Serial.printf("freeHeap=%u\n", ESP.getFreeHeap());
});
```

---

### `registerNow()`

```cpp
bool registerNow()
```

Forces an immediate registration with the dashboard server, regardless of the periodic interval.

- Returns `true` if the server responded with HTTP 2xx.
- Returns `false` if WiFi is not connected, the server URL is empty, or the request fails.

**Example:**
```cpp
if (!dash.registerNow()) {
  Serial.println("Registration failed");
}
```

---

### `setRegisterInterval()`

```cpp
void setRegisterInterval(unsigned long intervalMs)
```

Sets the periodic re-registration interval.

| Parameter    | Description                                                      |
|--------------|------------------------------------------------------------------|
| `intervalMs` | Interval in milliseconds. Default: `900000` (15 minutes).       |

**Example:**
```cpp
// Re-register every 5 minutes
dash.setRegisterInterval(5UL * 60UL * 1000UL);
```

---

### `setEepromSize()`

```cpp
void setEepromSize(uint16_t size)
```

Updates the EEPROM size used by the `/eeprom` endpoints. Has effect only if called **before** `begin()`.

---

### `buildInfoJson()`

```cpp
String buildInfoJson()
```

Returns a JSON string containing all system information for the device. Useful for debugging or for manually sending data to the server.

The JSON includes: MAC address, IP, hostname, local port, chip ID, CPU frequency, free heap, flash sizes, SDK version, last reset reason, RSSI, SSID, uptime, firmware version, LittleFS status, and the list of registered commands.

---

## HTTP Endpoints

The local HTTP server exposes the following endpoints. All paths are relative to the device IP on the port specified in the constructor.

### Always active

| Method | Path              | Description                                                              |
|--------|-------------------|--------------------------------------------------------------------------|
| GET    | `/`               | Plain-text response with the device name.                                |
| GET    | `/ping`           | JSON with `ok`, `id` (MAC), `uptime`, `freeHeap`, `rssi`.               |
| GET    | `/info`           | Full system info JSON (same payload as `buildInfoJson()`).               |
| POST   | `/cmd`            | Execute a command. JSON body: `{"command": "command_name"}`.             |
| GET    | `/eeprom`         | Read the entire EEPROM. Response: `{"ok":true,"size":N,"data":[...]}`.  |
| POST   | `/eeprom`         | Write the EEPROM. JSON body: `{"data":[byte0, byte1, ...]}`.            |
| POST   | `/eeprom/format`  | Erase the EEPROM (writes `0xFF` to all bytes).                          |
| POST   | `/update`         | OTA firmware upload (multipart). Reboots the device on success.         |

### Only when LittleFS is enabled

| Method     | Path           | Query parameter | Description                                            |
|------------|----------------|-----------------|--------------------------------------------------------|
| GET        | `/fs/info`     | —               | Total, used, and free filesystem space.                |
| GET        | `/fs/list`     | `path`          | List files and directories at the given path (default `/`). |
| GET        | `/fs/download` | `path`          | Download a file as an attachment.                      |
| DELETE/ANY | `/fs/delete`   | `path`          | Delete a file or directory.                            |
| POST       | `/fs/mkdir`    | `path`          | Create a directory.                                    |
| POST       | `/fs/upload`   | `path`          | Upload a file to the given path (multipart upload).    |

---

## ESP8266 / ESP32 Compatibility

The library handles platform differences internally via `#if defined(ESP32)`. No changes to user code are required when switching between platforms.

| Feature                 | ESP8266                    | ESP32                      |
|-------------------------|----------------------------|----------------------------|
| HTTP server             | `ESP8266WebServer`         | `WebServer`                |
| HTTP client             | `ESP8266HTTPClient`        | `HTTPClient`               |
| WiFi                    | `ESP8266WiFi`              | `WiFi`                     |
| Reset reason            | `ESP.getResetReason()`     | `esp_reset_reason()`       |
| Heap fragmentation      | Available                  | Not available              |
| Flash chip real size    | Available                  | Not available              |
| Chip ID                 | `ESP.getChipId()`          | `ESP.getEfuseMac()`        |
