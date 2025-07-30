#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ESP32Servo.h>

// Deep Sleep Parameters
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 1800ULL // 1 hour

// GPIO Pins
#define RESET_BUTTON_PIN D9
#define SERVO_POWER_PIN D7   // Controls the P-channel MOSFET gate (LOW to turn on)
#define SERVO_SIGNAL_PIN D8 // PWM signal for the servo

// Servo Angle Parameters
#define SERVO_HIGH_TIDE_ANGLE_1 0    // High Tide (left)
#define SERVO_LOW_TIDE_ANGLE 90      // Low Tide (bottom/center)
#define SERVO_HIGH_TIDE_ANGLE_2 180  // High Tide (right)

// NVS Preference Keys
const char* PREF_NAMESPACE = "tideClock";
const char* PREF_SSID = "wifi_ssid";
const char* PREF_NOAA_ID = "noaa_id";
const char* PREF_LAST_DOWNLOAD_DAY = "last_dl_day";
const char* PREF_NEXT_HIGH_TIDE_TIME = "next_ht_time";
const char* PREF_NEXT_HIGH_TIDE_HEIGHT = "next_ht_height";
const char* PREF_NEXT_LOW_TIDE_TIME = "next_lt_time";
const char* PREF_NEXT_LOW_TIDE_HEIGHT = "next_lt_height";

// WiFiManager and NVS Objects
WiFiManager wm;
Preferences preferences;
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org", -8 * 3600); // AKDT

// Global Configuration Variables
char wifi_ssid[60];
char noaa_station_id[10];
int lastDownloadDay = 0;

// Global variables for the *next* specific tide events
time_t nextHighTideTime = 0;
float nextHighTideHeight = 0.0;
time_t nextLowTideTime = 0;
float nextLowTideHeight = 0.0;

// Servo Object
Servo servoMotor;

// WiFiManager Custom Parameter
WiFiManagerParameter custom_noaa_station_id("stationid", "NOAA Station ID (e.g., 9455920)", "", 10);


// --- NVS Helper Functions (Unchanged) ---

void saveWifiConfiguration() {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString(PREF_SSID, wifi_ssid);
  preferences.end();
  Serial.println("Wi-Fi SSID saved to NVS.");
}

void saveNoaaStationId(const char* id) {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString(PREF_NOAA_ID, id);
  preferences.end();
  Serial.printf("NOAA Station ID '%s' saved to NVS.\n", id);
}

void saveLastDownloadDay(int day) {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putInt(PREF_LAST_DOWNLOAD_DAY, day);
  preferences.end();
  Serial.printf("lastDownloadDay %d saved to NVS.\n", day);
}

void loadConfiguration() {
  preferences.begin(PREF_NAMESPACE, true);
  preferences.getString(PREF_SSID, wifi_ssid, sizeof(wifi_ssid));
  preferences.getString(PREF_NOAA_ID, noaa_station_id, sizeof(noaa_station_id));
  lastDownloadDay = preferences.getInt(PREF_LAST_DOWNLOAD_DAY, 0);
  nextHighTideTime = preferences.getULong(PREF_NEXT_HIGH_TIDE_TIME, 0);
  nextHighTideHeight = preferences.getFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, 0.0);
  nextLowTideTime = preferences.getULong(PREF_NEXT_LOW_TIDE_TIME, 0);
  nextLowTideHeight = preferences.getFloat(PREF_NEXT_LOW_TIDE_HEIGHT, 0.0);
  preferences.end();
  
  Serial.print("Loaded SSID: "); Serial.println(wifi_ssid);
  Serial.print("Loaded NOAA ID: "); Serial.println(noaa_station_id);
  Serial.print("Loaded Last Download Day: "); Serial.println(lastDownloadDay);
  Serial.print("Loaded Next High Tide Time (Unix): "); Serial.println(nextHighTideTime);
  Serial.print("Loaded Next Low Tide Time (Unix): "); Serial.println(nextLowTideTime);
}


// --- WiFiManager Configuration (Unchanged) ---

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Config AP SSID: ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void setupWiFiManager() {
  wm.setDebugOutput(true);
  wm.setAPCallback(configModeCallback);
  wm.addParameter(&custom_noaa_station_id);
  wm.setConfigPortalTimeout(180);

  Serial.println("Attempting WiFiManager autoConnect...");
  if (!wm.autoConnect("TideClockSetupAP", "password")) {
    Serial.println("Failed to connect and hit timeout. Restarting...");
    ESP.restart();
  }
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  if (strcmp(wifi_ssid, WiFi.SSID().c_str()) != 0) {
    strcpy(wifi_ssid, WiFi.SSID().c_str());
    saveWifiConfiguration();
    Serial.println("WiFi SSID updated and saved.");
  }
  if (strcmp(noaa_station_id, custom_noaa_station_id.getValue()) != 0 && strlen(custom_noaa_station_id.getValue()) > 0) {
    strcpy(noaa_station_id, custom_noaa_station_id.getValue());
    saveNoaaStationId(noaa_station_id);
    Serial.println("NOAA Station ID updated and saved via portal.");
  } else if (strlen(noaa_station_id) == 0 && strlen(custom_noaa_station_id.getValue()) > 0) {
    strcpy(noaa_station_id, custom_noaa_station_id.getValue());
    saveNoaaStationId(noaa_station_id);
    Serial.println("Initial NOAA Station ID set and saved via portal.");
  }
}


// --- Time and NOAA Data Functions (Unchanged, but return condition for getNOAATideData is important) ---

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

void printLocalTime(time_t epochTime) {
    struct tm *timeinfo;
    timeinfo = localtime(&epochTime);
    Serial.print(asctime(timeinfo));
}

bool getNOAATideData() {
  if (strlen(noaa_station_id) == 0) {
    Serial.println("NOAA Station ID not set. Cannot download tide data. Please configure via WiFiManager.");
    return false;
  }

  HTTPClient http;
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?date=today&range=48&station=";
  url += noaa_station_id;
  url += "&product=predictions&datum=MLLW&time_zone=lst_ldt&interval=hilo&units=english&application=DataAPI_Sample&format=json";

  Serial.print("NOAA API URL: ");
  Serial.println(url);

  http.begin(url);
  int httpCode = http.GET();
  Serial.printf("HTTP GET response code: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.printf("NOAA Payload length: %d\n", payload.length());

    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed during NOAA data validation: "));
      Serial.println(error.f_str());
      http.end();
      return false;
    }
    JsonArray predictions = doc["predictions"];
    if (predictions.isNull() || predictions.size() == 0) {
      Serial.println("No predictions found or 'predictions' array is null/empty in NOAA response during validation.");
      http.end();
      return false;
    }

    // Reset global tide variables before populating them
    nextHighTideTime = 0; nextHighTideHeight = 0.0;
    nextLowTideTime = 0; nextLowTideHeight = 0.0;

    bool foundNextHigh = false;
    bool foundNextLow = false;
    time_t currentTime = ntpClient.getEpochTime();

    for (JsonObject prediction : predictions) {
        if (prediction.isNull()) continue;

        String type = prediction["type"].as<String>();
        String timeStr = prediction["t"].as<String>();
        float height = prediction["v"].as<float>();

        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &tm) == nullptr) {
             Serial.printf("  Skipping prediction: Failed to parse time string '%s'.\n", timeStr.c_str());
             continue;
        }
        time_t tideUnixTime = mktime(&tm);

        // Only consider tides in the future or very close to current time (e.g., within a minute or two)
        if (tideUnixTime >= currentTime - 120) {
            if (type == "H" && !foundNextHigh) {
                nextHighTideTime = tideUnixTime;
                nextHighTideHeight = height;
                foundNextHigh = true;
            } else if (type == "L" && !foundNextLow) {
                nextLowTideTime = tideUnixTime;
                nextLowTideHeight = height;
                foundNextLow = true;
            }
            if (foundNextHigh && foundNextLow) {
                break;
            }
        }
    }

    if (nextHighTideTime > 0) {
        Serial.print("Identified Next High Tide: "); printLocalTime(nextHighTideTime);
        Serial.print("  Height: "); Serial.println(nextHighTideHeight);
    } else {
        Serial.println("No future high tide identified in fetched data.");
    }
    if (nextLowTideTime > 0) {
        Serial.print("Identified Next Low Tide: "); printLocalTime(nextLowTideTime);
        Serial.print("  Height: "); Serial.println(nextLowTideHeight);
    } else {
        Serial.println("No future low tide identified in fetched data.");
    }

    preferences.begin(PREF_NAMESPACE, false);
    preferences.putULong(PREF_NEXT_HIGH_TIDE_TIME, nextHighTideTime);
    preferences.putFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, nextHighTideHeight);
    preferences.putULong(PREF_NEXT_LOW_TIDE_TIME, nextLowTideTime);
    preferences.putFloat(PREF_NEXT_LOW_TIDE_HEIGHT, nextLowTideHeight);
    preferences.end();
    
    Serial.println("Successfully downloaded and saved specific NOAA tide predictions.");
    // Return true only if *both* critical next tides were found.
    return foundNextHigh && foundNextLow; 

  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}


// --- Servo Control Function (Simplified Logic) ---

void updateServoPosition() {
    Serial.println("--- Entering updateServoPosition (Simplified Logic) ---");
    time_t currentTime = ntpClient.getEpochTime();
    
    int newAngle = SERVO_LOW_TIDE_ANGLE; // Default to a safe position

    time_t nextFutureTideTime = 0;
    String nextFutureTideType = "UNKNOWN"; // "H" or "L"

    // 1. Determine the absolute earliest future tide event.
    bool high_is_future = (nextHighTideTime > 0 && nextHighTideTime > currentTime);
    bool low_is_future = (nextLowTideTime > 0 && nextLowTideTime > currentTime);

    if (high_is_future && low_is_future) {
        if (nextHighTideTime < nextLowTideTime) {
            nextFutureTideTime = nextHighTideTime;
            nextFutureTideType = "H";
        } else {
            nextFutureTideTime = nextLowTideTime;
            nextFutureTideType = "L";
        }
    } else if (high_is_future) {
        nextFutureTideTime = nextHighTideTime;
        nextFutureTideType = "H";
    } else if (low_is_future) {
        nextFutureTideTime = nextLowTideTime;
        nextFutureTideType = "L";
    } else {
        Serial.println("No valid future tide data found. Cannot update servo position.");
        digitalWrite(SERVO_POWER_PIN, HIGH);
        if (servoMotor.attached()) servoMotor.detach();
        Serial.println("--- Exiting updateServoPosition (No valid future tide) ---");
        return;
    }

    Serial.printf("Next future tide is %s at %ld (%s)\n", 
                  nextFutureTideType.c_str(), nextFutureTideTime, asctime(localtime(&nextFutureTideTime)));

    // 2. Calculate "Time Until Next Tide" in minutes
    long timeUntilSeconds = nextFutureTideTime - currentTime;
    if (timeUntilSeconds < 0) timeUntilSeconds = 0; // Should not happen if logic above is correct, but safety.
    
    long timeUntilMinutes = timeUntilSeconds / 60;
    
    Serial.printf("Time until next tide: %ld seconds (%ld minutes)\n", timeUntilSeconds, timeUntilMinutes);

    // 3. Constrain the Time (Max 6 hours = 360 minutes)
    const int MAX_TIDE_CYCLE_MINUTES = 6 * 60; // 6 hours
    if (timeUntilMinutes > MAX_TIDE_CYCLE_MINUTES) {
        timeUntilMinutes = MAX_TIDE_CYCLE_MINUTES;
        Serial.printf("Time until constrained to %d minutes (6 hours)\n", MAX_TIDE_CYCLE_MINUTES);
    }

    // 4. Map to Angle
    if (nextFutureTideType == "L") {
        // Mapping for approaching Low Tide:
        //  timeUntilMinutes = 360 (6hrs)  -> Angle = 0 (High Tide Left)
        //  timeUntilMinutes = 0  (0hrs)   -> Angle = 90 (Low Tide Center)
        newAngle = map(timeUntilMinutes, MAX_TIDE_CYCLE_MINUTES, 0, SERVO_HIGH_TIDE_ANGLE_1, SERVO_LOW_TIDE_ANGLE);
        Serial.printf("Mapping for Low Tide: map(%ld, %d, 0, %d, %d) = %d\n",
                       timeUntilMinutes, MAX_TIDE_CYCLE_MINUTES, SERVO_HIGH_TIDE_ANGLE_1, SERVO_LOW_TIDE_ANGLE, newAngle);
    } else { // nextFutureTideType == "H"
        // Mapping for approaching High Tide:
        //  timeUntilMinutes = 360 (6hrs)  -> Angle = 90 (Low Tide Center)
        //  timeUntilMinutes = 0  (0hrs)   -> Angle = 180 (High Tide Right)
        newAngle = map(timeUntilMinutes, MAX_TIDE_CYCLE_MINUTES, 0, SERVO_LOW_TIDE_ANGLE, SERVO_HIGH_TIDE_ANGLE_2);
         Serial.printf("Mapping for High Tide: map(%ld, %d, 0, %d, %d) = %d\n",
                       timeUntilMinutes, MAX_TIDE_CYCLE_MINUTES, SERVO_LOW_TIDE_ANGLE, SERVO_HIGH_TIDE_ANGLE_2, newAngle);
    }

    // Final clamping to ensure valid servo range
    if (newAngle < 0) newAngle = 0;
    if (newAngle > 180) newAngle = 180;
    
    Serial.printf("Servo position calculated. Writing angle: %d\n", newAngle);
    
    // Power on, move, power off
    digitalWrite(SERVO_POWER_PIN, LOW); // Power on
    delay(50); // Small delay for servo power to stabilize
    servoMotor.attach(SERVO_SIGNAL_PIN);
    servoMotor.write(newAngle);
    delay(1000); // Wait for the servo to physically move
    servoMotor.detach();
    digitalWrite(SERVO_POWER_PIN, HIGH); // Power off
    
    Serial.println("--- Exiting updateServoPosition ---");
}

// --- Deep Sleep Function (Unchanged) ---

void deepSleep() {
  Serial.println("\n--- Entering deep sleep ---");

  time_t currentTime = ntpClient.getEpochTime();
  Serial.print("Current Time: ");
  printLocalTime(currentTime);

  Serial.println("\n--- Next Tide Info (from NVS) ---");
  time_t nextFutureTideTime = 0;
  String nextFutureTideType = "N/A";
  float nextFutureTideHeight = 0.0;

  bool high_is_future = (nextHighTideTime > currentTime);
  bool low_is_future = (nextLowTideTime > currentTime);

  if (high_is_future && low_is_future) {
    if (nextHighTideTime < nextLowTideTime) {
      nextFutureTideTime = nextHighTideTime;
      nextFutureTideType = "High";
      nextFutureTideHeight = nextHighTideHeight;
    } else {
      nextFutureTideTime = nextLowTideTime;
      nextFutureTideType = "Low";
      nextFutureTideHeight = nextLowTideHeight;
    }
  } else if (high_is_future) {
    nextFutureTideTime = nextHighTideTime;
    nextFutureTideType = "High";
    nextFutureTideHeight = nextHighTideHeight;
  } else if (low_is_future) {
    nextFutureTideTime = nextLowTideTime;
    nextFutureTideType = "Low";
    nextFutureTideHeight = nextLowTideHeight;
  }

  if (nextFutureTideTime != 0) {
    Serial.print("Next Tide ("); Serial.print(nextFutureTideType); Serial.print("): ");
    printLocalTime(nextFutureTideTime);
    Serial.print("Height: "); Serial.print(nextFutureTideHeight); Serial.println(" ft");

    long diffSeconds = nextFutureTideTime - currentTime;
    long hours = diffSeconds / 3600;
    long minutes = (diffSeconds % 3600) / 60;
    long seconds = diffSeconds % 60;

    Serial.printf("Time until next Tide: %ld hours, %ld minutes, %ld seconds\n", hours, minutes, seconds);
  } else {
    Serial.println("No future tide data available in NVS for display.");
    Serial.println("Will attempt to refresh data on next daily download cycle.");
  }
  Serial.println("-------------------------");

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}


// --- Arduino Setup and Loop (Revised `needsNewData` logic - minimal change) ---

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting Tide Clock...");

  pinMode(SERVO_POWER_PIN, OUTPUT);
  digitalWrite(SERVO_POWER_PIN, HIGH);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed at boot. Clearing all preferences and restarting.");
    preferences.begin(PREF_NAMESPACE, false);
    preferences.clear();
    preferences.end();
    wm.resetSettings();
    delay(1000);
    ESP.restart();
  }

  loadConfiguration();

  setupWiFiManager();

  getTimeFromNTP();

  time_t currentTime = ntpClient.getEpochTime();
  struct tm *ti = localtime(&currentTime);
  int currentDay = ti->tm_mday;

  Serial.printf("Current day: %d, Last download day: %d\n", currentDay, lastDownloadDay);
  Serial.printf("Current NOAA Station ID: %s\n", noaa_station_id);

  bool needsNewData = false;
  
  // CONDITION 1: New day (most common reason for refresh)
  if (currentDay != lastDownloadDay) {
      Serial.println("New day detected. Forcing new data download.");
      needsNewData = true;
  }
  
  // CONDITION 2: NOAA Station ID not set (first run or reset)
  if (strlen(noaa_station_id) == 0) {
      Serial.println("NOAA Station ID not set. Forcing new data download.");
      needsNewData = true;
  }

  // CONDITION 3: Critical tide data is missing (e.g., one or both are 0)
  if (nextHighTideTime == 0 || nextLowTideTime == 0) {
      Serial.println("One or both critical tide times (High/Low) are missing (0). Forcing new data download.");
      needsNewData = true;
  }

  // CONDITION 4: BOTH existing tide times are in the past relative to current time
  // (Ensures they are not 0 before checking if in past)
  if (nextHighTideTime > 0 && nextLowTideTime > 0 && 
      nextHighTideTime < currentTime && nextLowTideTime < currentTime) {
      Serial.println("Both existing next High and next Low tides from NVS are in the past. Forcing new data download.");
      needsNewData = true;
  }
  
  // CONDITION 5: Only ONE tide time is available and it's in the past (e.g. nextLowTideTime=0, nextHighTideTime in past)
  // This helps catch cases where `getNOAATideData` might have only found one tide last time.
  // Updated condition to ensure it's not trying to compare 0 to currentTime.
  if ((nextHighTideTime > 0 && nextHighTideTime < currentTime && nextLowTideTime == 0) ||
      (nextLowTideTime > 0 && nextLowTideTime < currentTime && nextHighTideTime == 0)) {
      Serial.println("One of the critical tide times is in the past, and the other is missing. Forcing new data download.");
      needsNewData = true;
  }


  if (needsNewData) {
    Serial.println("Attempting to download fresh tide data from NOAA...");
    // getNOAATideData() now returns true only if *both* tides are found.
    // If it fails to find both, needsNewData might remain true for the next cycle
    // to try again.
    if (getNOAATideData()) { 
      saveLastDownloadDay(currentDay);
      lastDownloadDay = currentDay;
      Serial.println("Tide data downloaded successfully and lastDownloadDay updated in NVS.");
    } else {
      Serial.println("Failed to download complete new tide data for the day. Servo might not function fully.");
      // No change to lastDownloadDay if download was incomplete/failed, so it tries again next time.
    }
  } else {
    Serial.println("Still same day, NOAA ID is set, and loaded tide data is current. Using previously downloaded tide data.");
  }

  updateServoPosition();
delay(20000);
  deepSleep();
}

void loop() {
  // This part of the code will not be reached
}