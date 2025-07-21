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

// GPIO Pin for the reset switch
#define RESET_BUTTON_PIN D9 // Connect button between D9 and GND

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

// Function to save Wi-Fi SSID to NVS
void saveWifiConfiguration() {
  preferences.begin(PREF_NAMESPACE, false); // R/W mode
  preferences.putString(PREF_SSID, wifi_ssid);
  preferences.end();
  Serial.println("Wi-Fi SSID saved to NVS.");
}

// Function to save lastDownloadDay to NVS
void saveLastDownloadDay(int day) {
  preferences.begin(PREF_NAMESPACE, false); // R/W mode
  preferences.putInt(PREF_LAST_DOWNLOAD_DAY, day);
  preferences.end();
  Serial.printf("lastDownloadDay %d saved to NVS.\n", day);
}

// Function to load configuration from NVS
void loadConfiguration() {
  preferences.begin(PREF_NAMESPACE, true); // Read-only mode
  preferences.getString(PREF_SSID, wifi_ssid, sizeof(wifi_ssid));
  lastDownloadDay = preferences.getInt(PREF_LAST_DOWNLOAD_DAY, 0); // Load lastDownloadDay
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

  wm.setConfigPortalTimeout(180);

  Serial.println("Attempting WiFiManager autoConnect...");

  if (!wm.autoConnect("TideClockSetupAP", "password")) { // Customize AP name and password
    Serial.println("Failed to connect and hit timeout. Restarting...");
    ESP.restart(); // Restart if autoConnect fails
  }

  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  if (strcmp(wifi_ssid, WiFi.SSID().c_str()) != 0) {
    strcpy(wifi_ssid, WiFi.SSID().c_str());
    saveWifiConfiguration(); // Use the dedicated save function for SSID
    Serial.println("WiFi SSID updated and saved.");
  }
}

// Function to get tide data from NOAA API
bool getNOAATideData() {
  HTTPClient http;
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?date=today&station=";
  url += HARDCODED_NOAA_STATION_ID;
  url += "&product=predictions&datum=MLLW&time_zone=lst_ldt&interval=hilo&units=english&application=DataAPI_Sample&format=json";

  Serial.print("NOAA API URL: ");
  Serial.println(url);

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(4000);
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

    nextHighTideTime = 0; nextHighTideHeight = 0.0;
    nextLowTideTime = 0; nextLowTideHeight = 0.0;

    for (JsonObject prediction : predictions) {
      String type = prediction["type"].as<String>();
      String timeStr = prediction["t"].as<String>();
      float height = prediction["v"].as<float>();

      struct tm tm;
      memset(&tm, 0, sizeof(tm));
      strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &tm);
      time_t tideUnixTime = mktime(&tm);

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
        if (foundNextHigh && foundNextLow) {
          break;
        }
      }
    }

    if (!foundNextHigh || !foundNextLow) {
        Serial.println("Warning: Could not find both next high and low tides for the current day/period.");
    }

    // Save the downloaded tide data to NVS
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putULong(PREF_NEXT_HIGH_TIDE_TIME, nextHighTideTime);
    preferences.putFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, nextHighTideHeight);
    preferences.putULong(PREF_NEXT_LOW_TIDE_TIME, nextLowTideTime);
    preferences.putFloat(PREF_NEXT_LOW_TIDE_HEIGHT, nextLowTideHeight);
    preferences.end(); // IMPORTANT: Close preferences after saving tide data

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

  time_t currentTime = ntpClient.getEpochTime();
  Serial.print("Current Time: ");
  printLocalTime(currentTime);

  Serial.println("\n--- Next Tide Info ---");
  time_t nextFutureTideTime = 0;
  String nextFutureTideType = "N/A";
  float nextFutureTideHeight = 0.0;

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
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}


void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting Tide Clock...");

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // Check if reset button is pressed during boot
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed at boot. Clearing preferences and restarting.");
    preferences.begin(PREF_NAMESPACE, false);
    preferences.clear(); // Clear all preferences in our namespace
    preferences.end();   // Commit the clear operation
    wm.resetSettings();  // Clear WiFiManager's stored credentials
    delay(1000);
    ESP.restart(); // Reboot to start fresh
  }

  // Load existing configuration and stored tide data
  loadConfiguration(); // This now loads lastDownloadDay correctly

  // Setup WiFi Manager (only for initial WiFi setup, NOAA ID is hardcoded)
  setupWiFiManager();

  // Get current time from NTP
  getTimeFromNTP();

  time_t currentTime = ntpClient.getEpochTime();
  struct tm *ti = localtime(&currentTime);
  int currentDay = ti->tm_mday; // Get current day of the month

  Serial.printf("Current day: %d, Last download day: %d\n", currentDay, lastDownloadDay);

  // Only download new data if it's a new day (currentDay != lastDownloadDay)
  // or if lastDownloadDay is 0 (which means it's the very first boot or after a reset)
  // This condition correctly handles both initial setup and daily refreshes.
  if (currentDay != lastDownloadDay) {
    Serial.println("New day detected or first run. Downloading fresh tide data from NOAA...");
    if (getNOAATideData()) {
      // If getNOAATideData is successful, save the current day to NVS
      saveLastDownloadDay(currentDay); // Use the dedicated function to save and close preferences
      lastDownloadDay = currentDay; // Also update the in-memory variable
      Serial.println("Tide data downloaded successfully and lastDownloadDay updated in NVS.");
    } else {
      Serial.println("Failed to download new tide data for the day. Will use existing data or show 'N/A'.");
      // If download fails, lastDownloadDay remains unchanged. This implies that on the next hourly wakeup
      // if currentDay != lastDownloadDay still holds, it will attempt to download again,
      // which is a good retry mechanism for daily data.
    }
  } else {
    Serial.println("Still same day. Using previously downloaded tide data.");
  }
  // No need for preferences.end() here, as specific save functions handle it.

  deepSleep();
}

void loop() {
  // This part of the code will not be reached as the ESP32 goes into deep sleep
  // from the setup() function and wakes up to restart setup().
}