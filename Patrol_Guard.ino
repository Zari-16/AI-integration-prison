/*
 Patrol_guard.ino (modified)
 - Sends JSON telemetry to the Edge ingestion endpoint (/ingest)
 - Edge performs inference and writes to InfluxDB
 - Keeps local DHT/MQ-135/PIR logic, but no longer calls sendToInflux locally
*/
#include <WiFiNINA.h>
#include "DHT.h"
#include <NTPClient.h>
#include <WiFiUdp.h>

#define DHTPIN A2
#define DHTTYPE DHT22

#define MQ135_PIN A1
#define WATER_PIN A0
#define PIR_PIN 7
#define BUZZER_PIN 6

DHT dht(DHTPIN, DHTTYPE);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// WiFi creds
char ssid[] = "Fairy";
char pass[] = "1608z246<3";

// Edge server config
const char EDGE_HOST[] = "192.168.1.50";
const uint16_t EDGE_PORT = 5000;
const char EDGE_API_KEY[] = "edge_api_key_here"; // match with server

const unsigned long LOOP_INTERVAL_MS = 5000UL; // 5 seconds
unsigned long lastLoop = 0;

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] Attempting reconnect...");
  long start = millis();
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED && (millis() - start < 15000)) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Reconnected.");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Reconnect timed out.");
  }
}

String readClientResponse(WiFiClient &c) {
  String resp = "";
  unsigned long start = millis();
  while (c.connected() || c.available()) {
    while (c.available()) {
      char ch = c.read();
      resp += ch;
      if (resp.length() > 8000) { resp += "\n[TRUNCATED]"; return resp; }
    }
    if (millis() - start > 2000) break;
  }
  return resp;
}

bool sendToEdge(float temperature, float humidity, int gasVal, int waterVal, int pirVal) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[Edge] WiFi not connected"); return false; }
  WiFiClient c;
  if (!c.connect(EDGE_HOST, EDGE_PORT)) {
    Serial.println("[Edge] Connect failed");
    return false;
  }
  unsigned long ts = timeClient.getEpochTime();
  String payload = "{\"device\":\"patrol_unit\",\"ts\":" + String(ts);
  payload += ",\"temp\":" + String(temperature,2);
  payload += ",\"humidity\":" + String(humidity,2);
  payload += ",\"gas\":" + String(gasVal);
  payload += ",\"water\":" + String(waterVal);
  payload += ",\"pir\":" + String(pirVal);
  payload += "}";
  String req = String("POST /ingest HTTP/1.1\r\n");
  req += "Host: " + String(EDGE_HOST) + ":" + String(EDGE_PORT) + "\r\n";
  req += "Authorization: Bearer " + String(EDGE_API_KEY) + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Content-Length: " + String(payload.length()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += payload;
  c.print(req);
  unsigned long start = millis();
  while (!c.available() && millis() - start < 1500) delay(10);
  String resp = "";
  while (c.available()) resp += (char)c.read();
  Serial.print("[Edge] Resp: "); Serial.println(resp);
  c.stop();
  return true;
}

void debugPrintHeader() {
  Serial.println(F("========================================"));
  Serial.print(F("Timestamp (ms): ")); Serial.println(millis());
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  delay(2000);
  Serial.println("Patrol Unit starting...");
  dht.begin();
  pinMode(MQ135_PIN, INPUT);
  pinMode(WATER_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) { Serial.println("\nWiFi connected"); }
  else { Serial.println("\nWiFi failed - will retry in loop"); }

  timeClient.begin();
  timeClient.update();
  lastLoop = millis();
}

void loop() {
  if (millis() - lastLoop < LOOP_INTERVAL_MS) return;
  lastLoop = millis();
  debugPrintHeader();
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Main] Skipping sensor read (no WiFi)");
    return;
  }
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int gasValue = analogRead(MQ135_PIN);
  int waterValue = analogRead(WATER_PIN);
  int pirValue = digitalRead(PIR_PIN);

  // Print sensor values
  if (isnan(temperature)) Serial.println(F("Temp: ERROR (NaN)")); else { Serial.print(F("Temp: ")); Serial.print(temperature, 2); Serial.println(F(" °C")); }
  if (isnan(humidity)) Serial.println(F("Humidity: ERROR (NaN)")); else { Serial.print(F("Humidity: ")); Serial.print(humidity, 2); Serial.println(F(" %")); }
  Serial.print(F("MQ-135 (raw): ")); Serial.println(gasValue);
  Serial.print(F("Water sensor (raw): ")); Serial.println(waterValue);
  Serial.print(F("PIR state (digital): ")); Serial.println(pirValue);

  bool alarmState = false;
  if (gasValue > 500) { Serial.println(F("[ALARM] High gas")); alarmState = true; }
  if (waterValue > 300) { Serial.println(F("[ALARM] Water detected")); alarmState = true; }

  if (alarmState) digitalWrite(BUZZER_PIN, HIGH);
  else digitalWrite(BUZZER_PIN, LOW);

  bool ok = sendToEdge(temperature, humidity, gasValue, waterValue, pirValue);
  if (ok) Serial.println(F("[Main] Sent data to Edge."));
  else Serial.println(F("[Main] Edge send failed."));

  Serial.println(F("Loop cycle complete."));
}
