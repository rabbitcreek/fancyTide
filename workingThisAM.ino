/*
  Tide Clock with NOAA API, Wi-Fi Config Portal, Deep Sleep, and Mobile-Friendly Web Interface
  For ESP32-S3 using Preferences to store tide data and Wi-Fi settings
  Last updated: July 10, 2025
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

Preferences preferences;
WebServer server(80);

String ssid = "";
String password = "";
String stationID = "9455920";  // Default: Anchorage, Alaska

struct TideEvent {
  time_t eventTime;
  String type; // "H" or "L"
};

#define MAX_EVENTS 10
TideEvent tideEvents[MAX_EVENTS];
int numEvents = 0;

String nextTideType = "";
int Diff = -1;

const char* timeZone = "AKST9AKDT,M3.2.0/2,M11.1.0/2"; // Alaska Time Zone

// ====================== WEB PAGE =======================
String getHTMLForm() {
  return R"rawliteral(
    <!DOCTYPE html><html>
    <head><meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial; font-size: 24px; margin: 40px; }
      input, select, button { font-size: 24px; padding: 10px; margin: 10px 0; width: 100%; }
    </style></head>
    <body>
      <h2>Configure Tide Clock</h2>
      <form action="/save" method="post">
        <label>WiFi SSID:</label><br>
        <input type="text" name="ssid"><br>
        <label>Password:</label><br>
        <input type="password" name="password"><br>
        <label>NOAA Station ID:</label><br>
        <input type="text" name="station" value="9455920"><br>
        <button type="submit">Save & Restart</button>
      </form>
    </body></html>
  )rawliteral";
}

String getConfirmationPage() {
  return R"rawliteral(
    <!DOCTYPE html><html>
    <head><meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial; font-size: 24px; text-align: center; margin: 40px; }
    </style></head>
    <body>
      <h2>Configuration Saved!</h2>
      <p>Your ESP will restart now and try to connect to WiFi.</p>
    </body></html>
  )rawliteral";
}

void handleRoot() {
  server.send(200, "text/html", getHTMLForm());
}

void handleSave() {
  ssid = server.arg("ssid");
  password = server.arg("password");
  stationID = server.arg("station");

  preferences.begin("config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("station", stationID);
  preferences.end();

  server.send(200, "text/html", getConfirmationPage());
  delay(2000);
  ESP.restart();
}

void setupWiFiConfigPortal() {
  WiFi.softAP("ESP32-Config");
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: "); Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();

  while (true) {
    server.handleClient();
    delay(10);
  }
}

bool loadWiFiCredentials() {
  preferences.begin("config", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  stationID = preferences.getString("station", "9455920");
  preferences.end();

  return (ssid.length() > 0);
}

void connectToWiFi() {
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) break;
    Serial.print("."); delay(500);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected.");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Failed to connect to WiFi.");
    setupWiFiConfigPortal();
  }
}

bool syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", timeZone, 1);
  tzset();

  Serial.println("Waiting for time sync...");
  for (int i = 0; i < 30; i++) {
    if (time(nullptr) > 1600000000) return true;
    delay(500);
  }
  return false;
}

bool isToday(time_t t) {
  struct tm *now = localtime(&t);
  struct tm nowDay = *now;
  time_t currentTime = time(nullptr);
  struct tm *cur = localtime(&currentTime);
  return (nowDay.tm_year == cur->tm_year && nowDay.tm_yday == cur->tm_yday);
}

void fetchTideData() {
  HTTPClient http;

  struct tm *tm_struct;
  time_t now = time(nullptr);
  tm_struct = localtime(&now);
  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y%m%d", tm_struct);

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?begin_date=" + String(dateStr) +
               "&range=24&station=" + stationID +
               "&product=predictions&datum=MLLW&time_zone=lst_ldt&units=english&interval=hilo&format=json";

  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);

    JsonArray arr = doc["predictions"].as<JsonArray>();
    numEvents = 0;
    for (JsonObject obj : arr) {
      String timeStr = obj["t"];
      String type = obj["type"];
      struct tm t;
      strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &t);
      tideEvents[numEvents].eventTime = mktime(&t);
      tideEvents[numEvents].type = type;
      numEvents++;
    }

    // Save to Preferences
    preferences.begin("tides", false);
    preferences.putBytes("events", tideEvents, sizeof(tideEvents));
    preferences.putInt("numEvents", numEvents);
    preferences.putULong("lastUpdate", now);
    preferences.end();

    Serial.println("Tide data updated from NOAA");
  } else {
    Serial.println("Failed to fetch tide data");
  }
  http.end();
}

bool loadTideDataFromPrefs() {
  preferences.begin("tides", true);
  preferences.getBytes("events", tideEvents, sizeof(tideEvents));
  numEvents = preferences.getInt("numEvents", 0);
  time_t lastUpdate = preferences.getULong("lastUpdate", 0);
  preferences.end();

  if (numEvents == 0 || !isToday(lastUpdate)) return false;
  return true;
}

void calculateNextTide() {
  time_t now = time(nullptr);
  Diff = -1;

  for (int i = 0; i < numEvents; i++) {
    if (tideEvents[i].eventTime > now) {
      Diff = (tideEvents[i].eventTime - now) / 3600;
      nextTideType = tideEvents[i].type;
      Serial.printf("Next tide: %s in %d hours\n", nextTideType.c_str(), Diff);
      return;
    }
  }
  Serial.println("No more tides today. Will fetch tomorrow's data next wake.");
}

void setup() {
  Serial.begin(115200);

  if (!loadWiFiCredentials()) setupWiFiConfigPortal();
  connectToWiFi();

  if (!syncTime()) {
    Serial.println("Time sync failed. Retrying next wake.");
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000);
    esp_deep_sleep_start();
  }

  if (!loadTideDataFromPrefs()) fetchTideData();

  calculateNextTide();

  Serial.printf("Going to sleep. Waking in 1 hour.\n\n");
  esp_sleep_enable_timer_wakeup(60ULL * 1000000);
  esp_deep_sleep_start();
}

void loop() {
  // never used
}
