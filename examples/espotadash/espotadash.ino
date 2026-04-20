/*
  ESPOTADASH - Basic Example
  ===========================
  Connects to WiFi, registers the device with the ESPOTADASH dashboard server,
  exposes a set of commands, and handles OTA firmware updates.

  Compatible with ESP8266 and ESP32.

  You need install the ESPOTADASH server and update the WiFi credentials and server URL
  You can find the server at https://github.com/neorob85/ESPOTADASH_server.git
*/

#include <Arduino.h>

#if defined(ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif

#include <ESPOTADASH.h>

// --- WiFi credentials ---
const char* WIFI_SSID     = "YourSSID";
const char* WIFI_PASSWORD = "YourPassword";

// --- Device settings ---
const char* DEVICE_NAME       = "my-esp-device";
const char* DASHBOARD_URL     = "http://192.168.1.100:3000";  // ESPOTADASH server URL
const char* OTA_PASSWORD      = "otasecret";
const char* FIRMWARE_VERSION  = "1.0.0";

// --- ESPOTADASH instance ---
// Parameters: HTTP port (default 80), EEPROM size in bytes, enable LittleFS
ESPOTADASH dash(80, 512, false);

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
#if !defined(ESP32)
  WiFi.hostname(DEVICE_NAME);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // --- Register commands ---
  // Each command appears as a button in the dashboard "Send Command" panel.

  dash.addCommand("reboot", "Restart the device", []() {
    delay(100);
    ESP.restart();
  });

  dash.addCommand("toggle_led", "Toggle the built-in LED", []() {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  });

  dash.addCommand("heap_report", "Print free heap to Serial", []() {
    Serial.printf("[CMD] freeHeap=%u\n", ESP.getFreeHeap());
  });

  // Start the library: registers with the server and starts the HTTP server + OTA
  dash.begin(DASHBOARD_URL, DEVICE_NAME, OTA_PASSWORD, FIRMWARE_VERSION);

  Serial.println("ESPOTADASH ready.");
}

void loop() {
  // Must be called in every loop iteration
  dash.loop();
}
