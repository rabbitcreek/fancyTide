#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <HTTPClient.h>
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <NTPClient.h>   // https://github.com/arduino-libraries/NTPClient
#include <WiFiUdp.h>
#include <Preferences.h> // ESP32 built-in library for NVS

// Deep Sleep
#define uS_TO_S_FACTOR 1000000ULL // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP 60ULL     // Time ESP32 will go to sleep (in seconds, 1 hour)

// Preference Keys
const char* PREF_NAMESPACE = "tideClock";
const char* PREF_SSID = "wifi_ssid";
const char* PREF_LAST_DOWNLOAD_DAY = "last_dl_day";
const char* PREF_NEXT_HIGH_TIDE_TIME = "next_ht_time";
const char* PREF_NEXT_LOW_TIDE_TIME = "next_lt_time";
const char* PREF_NEXT_HIGH_TIDE_HEIGHT = "next_ht_height";
const char* PREF_NEXT_LOW_TIDE_HEIGHT = "next_lt_height";

// Hardcoded NOAA Tide Station ID
const char* HARDCODED_NOAA_STATION_ID = "9455920"; // For demo purposes

WiFiManager wm;
Preferences preferences;
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org", -8 * 3600); // UTC-8 for AKDT, adjust if needed for your timezone

// Sleep-proof variables (stored in NVS or hardcoded)
char wifi_ssid[60];
int lastDownloadDay = 0; // Day of the month when data was last downloaded
time_t nextHighTideTime = 0;
float nextHighTideHeight = 0.0;
time_t nextLowTideTime = 0;
float nextLowTideHeight = 0.0;

// Function to save configuration to NVS
void saveConfiguration() {
  preferences.begin(PREF_NAMESPACE, false); // R/W mode
  preferences.putString(PREF_SSID, wifi_ssid);
  preferences.end();
  Serial.println("Configuration saved to NVS (SSID).");
}

// Function to load configuration from NVS
void loadConfiguration() {
  preferences.begin(PREF_NAMESPACE, true); // Read-only mode
  preferences.getString(PREF_SSID, wifi_ssid, sizeof(wifi_ssid));
  lastDownloadDay = preferences.getInt(PREF_LAST_DOWNLOAD_DAY, 0);
  nextHighTideTime = preferences.getULong(PREF_NEXT_HIGH_TIDE_TIME, 0);
  nextHighTideHeight = preferences.getFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, 0.0);
  nextLowTideTime = preferences.getULong(PREF_NEXT_LOW_TIDE_TIME, 0);
  nextLowTideHeight = preferences.getFloat(PREF_NEXT_LOW_TIDE_HEIGHT, 0.0);
  preferences.end();

  Serial.print("Loaded SSID: "); Serial.println(wifi_ssid);
  Serial.print("Loaded Last Download Day: "); Serial.println(lastDownloadDay);
  Serial.print("Loaded Next High Tide Time (Unix): "); Serial.println(nextHighTideTime);
  Serial.print("Loaded Next Low Tide Time (Unix): "); Serial.println(nextLowTideTime);
}

// Callback for when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Config AP SSID: ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

// Setup WiFiManager for initial configuration
void setupWiFiManager() {
  wm.setDebugOutput(true);
  wm.setAPCallback(configModeCallback);

  // No custom parameter for NOAA ID as it's hardcoded

  // Set timeout for configuration portal (e.g., 180 seconds)
  wm.setConfigPortalTimeout(180);

  Serial.println("Attempting WiFiManager autoConnect...");

  // Try to connect using saved credentials, or start configuration portal
  if (!wm.autoConnect("TideClockSetupAP", "password")) { // Customize AP name and password
    Serial.println("Failed to connect and hit timeout. Restarting...");
    ESP.restart(); // Restart if autoConnect fails
  }

  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Check if the SSID changed (WiFiManager updates its internal storage)
  if (strcmp(wifi_ssid, WiFi.SSID().c_str()) != 0) {
    strcpy(wifi_ssid, WiFi.SSID().c_str());
    saveConfiguration(); // Save the new SSID
    Serial.println("WiFi SSID updated and saved.");
  }
}

// Function to get tide data from NOAA API
bool getNOAATideData() {
  HTTPClient http;
  // Use the simpler URL with interval=hilo for direct high/low predictions
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?date=today&station=";
  url += HARDCODED_NOAA_STATION_ID; // Use the hardcoded ID
  url += "&product=predictions&datum=MLLW&time_zone=lst_ldt&interval=hilo&units=english&application=DataAPI_Sample&format=json";

  Serial.print("NOAA API URL: ");
  Serial.println(url);

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    // Serial.println("NOAA API Response:");
    // Serial.println(payload);

    DynamicJsonDocument doc(4000); // Adjust capacity as needed
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      http.end();
      return false;
    }

    JsonArray predictions = doc["predictions"];
    if (predictions.size() == 0) {
      Serial.println("No predictions found in NOAA response.");
      http.end();
      return false;
    }

    time_t currentUnixTime = ntpClient.getEpochTime();
    bool foundNextHigh = false;
    bool foundNextLow = false;

    // Clear previous tide data
    nextHighTideTime = 0;
    nextHighTideHeight = 0.0;
    nextLowTideTime = 0;
    nextLowTideHeight = 0.0;

    for (JsonObject prediction : predictions) {
      String type = prediction["type"].as<String>();
      String timeStr = prediction["t"].as<String>(); // Format: YYYY-MM-DD HH:MM
      float height = prediction["v"].as<float>();

      // Parse time string to time_t
      struct tm tm;
      memset(&tm, 0, sizeof(tm));
      strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &tm);
      time_t tideUnixTime = mktime(&tm);

      // Only consider future tides for 'next' designation.
      // We look for the first future high and first future low.
      if (tideUnixTime >= currentUnixTime) {
        if (type == "H" && !foundNextHigh) {
          nextHighTideTime = tideUnixTime;
          nextHighTideHeight = height;
          foundNextHigh = true;
          Serial.print("Next High Tide found: ");
          Serial.print(asctime(localtime(&nextHighTideTime)));
          Serial.print("Height: "); Serial.println(nextHighTideHeight);
        } else if (type == "L" && !foundNextLow) {
          nextLowTideTime = tideUnixTime;
          nextLowTideHeight = height;
          foundNextLow = true;
          Serial.print("Next Low Tide found: ");
          Serial.print(asctime(localtime(&nextLowTideTime)));
          Serial.print("Height: "); Serial.println(nextLowTideHeight);
        }

        // Once both the next high and next low tide *in the future* are found, we can stop.
        if (foundNextHigh && foundNextLow) {
          break;
        }
      }
    }

    if (!foundNextHigh || !foundNextLow) {
        Serial.println("Warning: Could not find both next high and low tides for the current day/period.");
        // If not both are found, it might mean the day is ending and one type is already past.
        // The display logic will handle showing "No future tide data" if the times are 0 or past.
    }

    // Save the downloaded tide data to NVS
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putULong(PREF_NEXT_HIGH_TIDE_TIME, nextHighTideTime);
    preferences.putFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, nextHighTideHeight);
    preferences.putULong(PREF_NEXT_LOW_TIDE_TIME, nextLowTideTime);
    preferences.putFloat(PREF_NEXT_LOW_TIDE_HEIGHT, nextLowTideHeight);
    preferences.end();

    http.end();
    return true;

  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}

// Function to get current time from NTP
void getTimeFromNTP() {
  Serial.println("Getting time from NTP...");
  ntpClient.begin();
  if (!ntpClient.forceUpdate()) {
    Serial.println("Failed to get time from NTP server.");
  } else {
    Serial.print("Current NTP time: ");
    Serial.println(ntpClient.getFormattedTime());
  }
}

// Helper to print time
void printLocalTime(time_t epochTime) {
    struct tm *timeinfo;
    timeinfo = localtime(&epochTime);
    Serial.print(asctime(timeinfo));
}

// Function to put ESP32 into deep sleep
void deepSleep() {
  Serial.println("\n--- Entering deep sleep ---");

  // Print current tide information
  time_t currentTime = ntpClient.getEpochTime();
  Serial.print("Current Time: ");
  printLocalTime(currentTime);

  Serial.println("\n--- Next Tide Info ---");
  // Determine which is the *very next* tide (high or low)
  time_t nextFutureTideTime = 0;
  String nextFutureTideType = "N/A";
  float nextFutureTideHeight = 0.0;

  // Check which tide is sooner and still in the future
  if (nextHighTideTime > currentTime && nextLowTideTime > currentTime) {
    if (nextHighTideTime < nextLowTideTime) {
      nextFutureTideTime = nextHighTideTime;
      nextFutureTideType = "High";
      nextFutureTideHeight = nextHighTideHeight;
    } else {
      nextFutureTideTime = nextLowTideTime;
      nextFutureTideType = "Low";
      nextFutureTideHeight = nextLowTideHeight;
    }
  } else if (nextHighTideTime > currentTime) {
    nextFutureTideTime = nextHighTideTime;
    nextFutureTideType = "High";
    nextFutureTideHeight = nextHighTideHeight;
  } else if (nextLowTideTime > currentTime) {
    nextFutureTideTime = nextLowTideTime;
    nextFutureTideType = "Low";
    nextFutureTideHeight = nextLowTideHeight;
  }

  if (nextFutureTideTime != 0) {
    Serial.print("Next Tide ("); Serial.print(nextFutureTideType); Serial.print("): ");
    printLocalTime(nextFutureTideTime);
    Serial.print("Height: "); Serial.print(nextFutureTideHeight); Serial.println(" ft (English units)");

    long diffSeconds = nextFutureTideTime - currentTime;
    long hours = diffSeconds / 3600;
    long minutes = (diffSeconds % 3600) / 60;
    long seconds = diffSeconds % 60;

    Serial.printf("Time until next Tide: %ld hours, %ld minutes, %ld seconds\n", hours, minutes, seconds);
  } else {
    Serial.println("No future tide data available for display or calculation within the fetched range.");
    Serial.println("Will attempt to refresh data on next daily download cycle.");
  }
  Serial.println("-------------------------");

delay(20000);
  // Set deep sleep timer
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}


void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting Tide Clock...");

  preferences.begin(PREF_NAMESPACE, false); // Open preferences in R/W mode

  // Load existing configuration and stored tide data
  loadConfiguration();

  // Setup WiFi Manager (only for initial WiFi setup, NOAA ID is hardcoded)
  setupWiFiManager();

  // Get current time from NTP
  getTimeFromNTP();

  // Check if it's a new day for data download
  time_t currentTime = ntpClient.getEpochTime();
  struct tm *ti = localtime(&currentTime);
  int currentDay = ti->tm_mday;

  Serial.printf("Current day: %d, Last download day: %d\n", currentDay, lastDownloadDay);

  // **** KEY POWER-SAVING CHANGE HERE ****
  // Only download new data if it's a new day.
  // We no longer trigger a download just because stored tides are in the past.
  if (currentDay != lastDownloadDay) {
    Serial.println("New day detected. Downloading fresh tide data from NOAA...");
    if (getNOAATideData()) {
      preferences.putInt(PREF_LAST_DOWNLOAD_DAY, currentDay);
      lastDownloadDay = currentDay; // Update in-memory variable as well
      Serial.println("Tide data downloaded successfully and saved for the day.");
    } else {
      Serial.println("Failed to download new tide data for the day. Will use existing data.");
      // If download fails, lastDownloadDay remains unchanged, so it will try again next hour
      // if currentDay != lastDownloadDay. This might be undesirable if internet is flaky.
      // For truly "once per day", you might putInt(currentDay) even on failure,
      // but then you risk using stale data for a full day if the first download fails.
      // Sticking to "only on new day" for download attempts is safest for battery.
    }
  } else {
    Serial.println("Still same day. Using previously downloaded tide data.");
  }

  // Now, go to deep sleep
  deepSleep();
}

void loop() {
  // This part of the code will not be reached as the ESP32 goes into deep sleep
  // from the setup() function and wakes up to restart setup().
}