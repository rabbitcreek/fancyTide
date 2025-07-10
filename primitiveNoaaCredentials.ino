#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

Preferences prefs;
WebServer server(80);

String ssid, password, stationId;
bool configMode = false;

// Serve config form
void handleRoot() {
  String page = "<html><body><h2>ESP32 NOAA Config</h2><form method='POST' action='/save'>";
  page += "WiFi SSID: <input name='ssid'><br>";
  page += "WiFi Password: <input name='password' type='password'><br>";
  page += "NOAA Station ID: <input name='station'><br>";
  page += "<input type='submit'></form></body></html>";
  server.send(200, "text/html", page);
}

// Handle config form submission
void handleSave() {
  ssid = server.arg("ssid");
  password = server.arg("password");
  stationId = server.arg("station");

  prefs.begin("config", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.putString("station", stationId);
  prefs.end();

  server.send(200, "text/html", "<html><body><h2>Saved! Rebooting...</h2></body></html>");
  delay(3000);
  ESP.restart();
}

// Start Access Point for setup
void startConfigPortal() {
  configMode = true;
  WiFi.softAP("TideSetupESP32");

  IPAddress IP = WiFi.softAPIP();
  Serial.println("AP IP address: " + IP.toString());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Config portal started");
}

// Get formatted date for API
String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time error");
    return "20250709";  // fallback
  }
  char buffer[9];
  strftime(buffer, sizeof(buffer), "%Y%m%d", &timeinfo);
  return String(buffer);
}

// NOAA API request
void fetchTides(String station) {
  String date = getCurrentDate();
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter";
  url += "?product=predictions&application=arduino_tide_reader";
  url += "&begin_date=" + date + "&end_date=" + date;
  url += "&datum=MLLW&station=" + station;
  url += "&time_zone=lst_ldt&units=english&interval=hilo&format=json";

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("JSON Error: ");
      Serial.println(error.c_str());
      return;
    }

    Serial.println("Tide Predictions for Station " + station);
    for (JsonObject tide : doc["predictions"].as<JsonArray>()) {
      String time = tide["t"];
      String type = tide["type"];
      String height = tide["v"];
      Serial.printf("  %s tide at %s - %.2f ft\n",
                    type == "H" ? "High" : "Low",
                    time.c_str(),
                    height.toFloat());
    }
  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  prefs.begin("config", true);
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("password", "");
  stationId = prefs.getString("station", "");
  prefs.end();

  if (ssid == "" || password == "" || stationId == "") {
    Serial.println("No saved config. Starting setup mode.");
    startConfigPortal();
  } else {
    Serial.println("Connecting to saved WiFi...");
    WiFi.begin(ssid.c_str(), password.c_str());

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
      delay(500);
      Serial.print(".");
      timeout++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected");
      configTime(-8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
      delay(2000);
      fetchTides(stationId);
    } else {
      Serial.println("\nWiFi failed. Starting config mode.");
      startConfigPortal();
    }
  }
}

void loop() {
  if (configMode) server.handleClient();
}
