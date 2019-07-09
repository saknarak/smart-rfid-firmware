#include <ArduinoJson.h>
#include <SPIFFS.h>

#define CONFIG_FILE "/config.json"
// data address 0x003d1000
typedef struct {
  char deviceCode[13]; // mac
  char apPass[64];
  char ssid[64];
  char pass[64];
  char apiUrl[64];
} config_t;

config_t cfg;

#include <WiFi.h>
#include <HTTPClient.h>
int wifiStatus = 0; // 0 disconnected, 1 connecting, 2 connected


#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "AsyncJson.h"

#include <SPI.h>
#include <MFRC522.h>
#define RFID_RST_PIN 0
#define RFID_SCK_PIN 17
#define RFID_MISO_PIN 23
#define RFID_MOSI_PIN 22
#define RFID_SS_PIN 21

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN); 
AsyncWebServer server(80);
unsigned long restartTimer = 0;
byte mac[6];

void setup() {
  Serial.begin(115200);
  SPIFFS.begin();
  
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  loadConfig();
  dumpConfigFile();
  wifiSetup();
  serverSetup();
  rfidSetup();
}

void loop() {
  if (restartTimer > 0 && millis() >= restartTimer) {
    ESP.restart();
  }
  wifiLoop();
  rfidLoop();
}

void loadConfig() {
  // load json
  Serial.println("BEGIN loadConfig");
  File file = SPIFFS.open(CONFIG_FILE, "r"); // 4MB
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println(F("Failed to read file, using default configuration"));
  }
  // { ssid: "", pass: "", apiUrl: "", port: 7009 }
  strlcpy(cfg.apPass, doc["apPass"] | "12345678", sizeof(cfg.apPass));
  strlcpy(cfg.ssid, doc["ssid"] | "websquare_2.4G", sizeof(cfg.ssid));
  strlcpy(cfg.pass, doc["pass"] | "0891560526", sizeof(cfg.pass));
  strlcpy(cfg.apiUrl, doc["apiUrl"] | "http://192.168.1.117:7009/api/card/log", sizeof(cfg.apiUrl));
  file.close();
  if (error) {
    saveConfig();
  }
}

void saveConfig() {
  Serial.println("BEGIN saveConfig");
  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) {
    Serial.println(F("Failed to create file, format SPIFFS"));
    SPIFFS.format();
    file = SPIFFS.open(CONFIG_FILE, "w");
    if (!file) {
      Serial.println(F("Failed to create file"));
      return;
    }
  }

  StaticJsonDocument<512> doc;
  doc["apPass"] = cfg.apPass;
  doc["ssid"] = cfg.ssid;
  doc["pass"] = cfg.pass;
  doc["apiUrl"] = cfg.apiUrl;

  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  }
  file.close();
  Serial.println("END saveConfig");
}

void mergeConfig(JsonObject& doc) {
  if (doc.containsKey("apPass")) {
    strlcpy(cfg.apPass, doc["apPass"], sizeof(cfg.apPass));
  }
  if (doc.containsKey("ssid")) {
    strlcpy(cfg.ssid, doc["ssid"], sizeof(cfg.ssid));
  }
  if (doc.containsKey("pass")) {
    strlcpy(cfg.pass, doc["pass"], sizeof(cfg.pass));
  }
  if (doc.containsKey("apiUrl")) {
    strlcpy(cfg.apiUrl, doc["apiUrl"], sizeof(cfg.apiUrl));
  }
}
void dumpConfigFile() {
  File f = SPIFFS.open(CONFIG_FILE, "r");
  Serial.println("BEGIN config ========");
  while (f.available()){
    Serial.write(f.read());
  }
  f.close();
  Serial.println("\nEND config =========");
}

void serverSetup() {
  server.serveStatic("/", SPIFFS, "/html").setDefaultFile("index.html");
  // / => /html/index.html
  // /main.css => /html/main.css
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "<h1>Hello, world</h1>");
  });
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, CONFIG_FILE, "application/json");
  });
  AsyncCallbackJsonWebHandler* handler = new AsyncCallbackJsonWebHandler("/config", [](AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject doc = json.as<JsonObject>();
    mergeConfig(doc);
    saveConfig();
    request->send(200, "application/json", "{\"ok\":1}");
    restartTimer = millis() + 1000;
  });
  server.addHandler(handler);
  server.begin();
}

void wifiSetup() {
  char apName[32];
  sprintf(apName, "SMART-%02X%02X%02X", mac[3], mac[4], mac[5]);
  WiFi.softAP(apName, cfg.apPass);
  WiFi.begin(cfg.ssid, cfg.pass);
  wifiStatus = 1;
}

void wifiLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiStatus == 2) {
      // wifi disconnected
      Serial.println("wifi disconnected");
      wifiSetup();
    }
  } else {
    if (wifiStatus != 2) {
      Serial.println("wifi connected");
      Serial.print("IP=");
      Serial.println(WiFi.localIP());
      wifiStatus = 2;
    }
  }
}

void rfidSetup() {
  Serial.println("rfidSetup begin");
  pinMode(RFID_RST_PIN, OUTPUT);
  digitalWrite(RFID_RST_PIN, 0);
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  rfid.PCD_DumpVersionToSerial();
  Serial.println("rfidSetup end");
}

void rfidLoop() {
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  Serial.println("found new card");
  if ( ! rfid.PICC_ReadCardSerial()) {
    return;
  }

  Serial.print("uid size=");
  Serial.println(rfid.uid.size);
  Serial.print("uid=");
  char uid[21];
  for (int i = 0; i < rfid.uid.size; i++) { // 4, 7, 10
    sprintf(uid + i*2, "%02X", rfid.uid.uidByte[i]);
    Serial.printf("%02X", rfid.uid.uidByte[i]);
  }

  Serial.println();
  rfid.PICC_HaltA();

  HTTPClient http;
  http.begin(cfg.apiUrl);
  char payload[100];
  sprintf(payload, "{\"cardCode\":\"%s\",\"readerCode\":\"R001\"}", uid);
  http.addHeader("Content-Type", "application/json"); 
  int httpCode = http.POST(payload);
  Serial.printf("httpCode=%d\n", httpCode);
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.print("GOT:");
      Serial.println(payload);
    }
  }
  http.end();  
}
