#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define RESET_BUTTON_PIN 0

Preferences prefs;
WebServer server(80);

String ssid, password, stationId;
bool configMode = false;

void handleRoot() {
  String page = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>ESP32 NOAA Setup</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body {
          font-family: Arial, sans-serif;
          text-align: center;
          background-color: #f0f8ff;
          margin: 0;
          padding: 0;
        }
        h2 {
          font-size: 2em;
          margin-top: 1em;
        }
        form {
          display: flex;
          flex-direction: column;
          align-items: center;
          padding: 1em;
        }
        input[type="text"],
        input[type="password"] {
          font-size: 1.2em;
          width: 80%;
          max-width: 400px;
          padding: 10px;
          margin: 10px 0;
          border-radius: 8px;
          border: 1px solid #aaa;
        }
        input[type="submit"] {
          font-size: 1.4em;
          padding: 12px 24px;
          margin-top: 20px;
          border-radius: 8px;
          background-color: #007bff;
          color: white;
          border: none;
          cursor: pointer;
        }
        input[type="submit"]:hover {
          background-color: #0056b3;
        }
      </style>
    </head>
    <body>
      <h2>ESP32 NOAA Config</h2>
      <form method="POST" action="/save">
        <input name="ssid" type="text" placeholder="WiFi SSID" required><br>
        <input name="password" type="password" placeholder="WiFi Password" required><br>
        <input name="station" type="text" placeholder="NOAA Station ID" required><br>
        <input type="submit" value="Save and Connect">
      </form>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", page);
}

void handleSave() {
  ssid = server.arg("ssid");
  password = server.arg("password");
  stationId = server.arg("station");

  prefs.begin("config", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.putString("station", stationId);
  prefs.end();

  String page = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>ESP32 Saved</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: Arial; text-align: center; background-color: #e6fff2; }
        h2 { color: #007b55; margin-top: 1em; }
        p { font-size: 1.2em; }
        .info { font-weight: bold; }
      </style>
      <script>
        setTimeout(function() {
          window.location.href = "/reboot";
        }, 3000);
      </script>
    </head>
    <body>
      <h2>\u2705 Settings Saved!</h2>
      <p>WiFi: <span class='info'>%SSID%</span></p>
      <p>NOAA Station ID: <span class='info'>%STATION%</span></p>
      <p>Rebooting in 3 seconds...</p>
    </body>
    </html>
  )rawliteral";

  page.replace("%SSID%", ssid);
  page.replace("%STATION%", stationId);

  server.send(200, "text/html", page);
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(1000);
  ESP.restart();
}

void showConfigForm() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/generate_204", []() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
  server.on("/hotspot-detect.html", []() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
  server.onNotFound([]() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
  server.on("/reboot", handleReboot);
  server.begin();
  Serial.println("Config portal running");
}

void startConfigPortal() {
  configMode = true;
  WiFi.softAP("TideSetupESP32");
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  showConfigForm();
}

String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "20250709";
  }
  char buffer[9];
  strftime(buffer, sizeof(buffer), "%Y%m%d", &timeinfo);
  return String(buffer);
}

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
      Serial.println("JSON Error");
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
    Serial.println("HTTP error: " + String(httpCode));
  }
  http.end();
}

void clearPreferences() {
  prefs.begin("config", false);
  prefs.clear();
  prefs.end();
  Serial.println("Preferences cleared. Restarting...");
  delay(2000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button held - clearing preferences.");
    clearPreferences();
  }

  prefs.begin("config", true);
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("password", "");
  stationId = prefs.getString("station", "");
  prefs.end();

  if (ssid == "" || password == "" || stationId == "") {
    Serial.println("No config. Starting AP.");
    startConfigPortal();
  } else {
    Serial.println("Connecting to WiFi...");
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
      server.on("/config", showConfigForm);
      server.begin();
      fetchTides(stationId);
    } else {
      Serial.println("\nWiFi failed. Starting AP.");
      startConfigPortal();
    }
  }
}

void loop() {
  if (configMode || WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }
}