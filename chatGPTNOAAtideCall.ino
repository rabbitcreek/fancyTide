#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Replace with your WiFi credentials
const char* ssid = "werner";
const char* password = "9073456071";

// NOAA Station ID for Anchorage, AK
const char* stationId = "9455920";

// Helper to get current date (YYYYMMDD)
String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return "20250709";  // fallback
  }
  char buffer[9];
  strftime(buffer, sizeof(buffer), "%Y%m%d", &timeinfo);
  return String(buffer);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  // Set timezone (Alaska Daylight Time: UTC-8 or UTC-9)
  configTime(-8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  delay(2000); // allow time sync

  String date = getCurrentDate();
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter";
  url += "?product=predictions&application=arduino_tide_reader";
  url += "&begin_date=" + date + "&end_date=" + date;
  url += "&datum=MLLW&station=" + String(stationId);
  url += "&time_zone=lst_ldt&units=english&interval=hilo&format=json";

  Serial.println("Requesting: " + url);

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();

      // Parse JSON
      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.print("JSON error: ");
        Serial.println(error.c_str());
        return;
      }

      JsonArray predictions = doc["predictions"];
      Serial.println("High/Low Tides for Today:");
      for (JsonObject tide : predictions) {
        String time = tide["t"];
        String type = tide["type"];
        String height = tide["v"];
        Serial.printf("  %s tide at %s - %.2f ft\n",
                      type == "H" ? "High" : "Low",
                      time.c_str(),
                      height.toFloat());
      }
    }
  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
  }

  http.end();
}

void loop() {
  // nothing in loop
}
