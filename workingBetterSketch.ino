#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

#define SERVO_PIN 2
#define RESET_PIN 0

const char* default_ssid = "ESP_TideClock";
const char* default_password = "12345678";
const char* config_portal_title = "Tide Clock Setup";

Preferences preferences;

String wifiSSID;
String wifiPassword;
String stationId;

struct TideEvent {
  time_t timestamp;
  String type;
};

TideEvent tideEvents[10];
int tideCount = 0;
int storedDay = -1;

void connectToWiFi() {
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.print("Connecting to WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
  } else {
    Serial.println("\nFailed to connect to WiFi.");
  }
}

void setupTime() {
  configTime(-8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(100);
    now = time(nullptr);
  }
  struct tm* lt = localtime(&now);
  Serial.printf("Current local time: %04d-%02d-%02d %02d:%02d:%02d\n", 
    lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
}

void fetchTides() {
  HTTPClient http;
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&application=esp32tideclock&begin_date=today&range=24&station=";
  url += stationId;
  url += "&time_zone=lst_ldt&units=english&interval=hilo&format=json";

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("Tide JSON:");
    Serial.println(payload);

    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);
    JsonArray predictions = doc["predictions"];

    tideCount = 0;
    for (JsonObject pred : predictions) {
      String t = pred["t"];
      String type = pred["type"];

      struct tm tm;
      strptime(t.c_str(), "%Y-%m-%d %H:%M", &tm);
      tideEvents[tideCount].timestamp = mktime(&tm);
      tideEvents[tideCount].type = type;
      Serial.printf("Tide %d: %s - %s\n", tideCount, t.c_str(), type.c_str());
      tideCount++;
      if (tideCount >= 10) break;
    }

    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    storedDay = lt->tm_mday;

    preferences.putBytes("tides", &tideEvents, sizeof(tideEvents));
    preferences.putInt("tideCount", tideCount);
    preferences.putInt("storedDay", storedDay);
  } else {
    Serial.println("Failed to fetch tide data");
  }
  http.end();
}

void loadStoredTides() {
  tideCount = preferences.getInt("tideCount", 0);
  storedDay = preferences.getInt("storedDay", -1);
  preferences.getBytes("tides", &tideEvents, sizeof(tideEvents));
}

void setup() {
  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);
  preferences.begin("tideclock", false);

  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");
  stationId = preferences.getString("station", "9455920");

  if (wifiSSID == "" || digitalRead(RESET_PIN) == LOW) {
    // Code for config portal would go here
    // Skipped for brevity
    Serial.println("Config portal would run here.");
  }

  connectToWiFi();
  setupTime();
  time_t now = time(nullptr);
  struct tm* lt = localtime(&now);

  if (tideCount == 0 || storedDay != lt->tm_mday) {
    fetchTides();
  } else {
    loadStoredTides();
  }

  // Find next tide event
  TideEvent nextTide;
  for (int i = 0; i < tideCount; i++) {
    if (tideEvents[i].timestamp > now) {
      nextTide = tideEvents[i];
      break;
    }
  }

  float diffHours = difftime(nextTide.timestamp, now) / 3600.0;
  Serial.printf("Time to next tide: %.2f hours (%s)\n", diffHours, nextTide.type.c_str());

  // Example servo logic (user will implement actual servo movement)
  // moveServoBasedOnTide(diffHours, nextTide.type);

  // Sleep until next hour
  esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  Serial.println("Going to sleep...");
  delay(100);
  esp_deep_sleep_start();
}

void loop() {
  // unused
}
