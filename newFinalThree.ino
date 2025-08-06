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
const char* PREF_LAST_HIGH_TIDE_TIME = "last_ht_time";
const char* PREF_LAST_LOW_TIDE_TIME = "last_lt_time";

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
time_t lastHighTideTime = 0;
time_t lastLowTideTime = 0;

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
  lastHighTideTime = preferences.getULong(PREF_LAST_HIGH_TIDE_TIME, 0);
  lastLowTideTime = preferences.getULong(PREF_LAST_LOW_TIDE_TIME, 0);
  preferences.end();
  
  Serial.print("Loaded SSID: "); Serial.println(wifi_ssid);
  Serial.print("Loaded NOAA ID: "); Serial.println(noaa_station_id);
  Serial.print("Loaded Last Download Day: "); Serial.println(lastDownloadDay);
  Serial.print("Loaded Next High Tide Time (Unix): "); Serial.println(nextHighTideTime);
  Serial.print("Loaded Next Low Tide Time (Unix): "); Serial.println(nextLowTideTime);
  Serial.print("Loaded Last High Tide Time (Unix): "); Serial.println(lastHighTideTime);
  Serial.print("Loaded Last Low Tide Time (Unix): "); Serial.println(lastLowTideTime);
}


// --- WiFiManager Configuration (Corrected logic) ---

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Config AP SSID: ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

bool connectToWiFi() {
  Serial.println("Attempting WiFiManager autoConnect...");
  wm.setDebugOutput(true);
  wm.setAPCallback(configModeCallback);
  wm.addParameter(&custom_noaa_station_id);

  // Set a short timeout for the initial connection attempt.
  // We want to fail fast if the network is down.
  wm.setConnectTimeout(10);
  
  if (wm.autoConnect("TideClockSetupAP", "password")) {
    Serial.println("WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Check if the configuration has been updated via the portal
    if (strcmp(noaa_station_id, custom_noaa_station_id.getValue()) != 0 && strlen(custom_noaa_station_id.getValue()) > 0) {
      strcpy(noaa_station_id, custom_noaa_station_id.getValue());
      saveNoaaStationId(noaa_station_id);
      Serial.println("NOAA Station ID updated and saved via portal.");
    }
    return true;
  } else {
    Serial.println("Failed to connect to saved network or portal timed out.");
    return false;
  }
}


// --- The rest of the program remains unchanged ---

bool getTimeFromNTP() {
  Serial.println("Getting time from NTP...");
  ntpClient.begin();
  if (!ntpClient.forceUpdate()) {
    Serial.println("Failed to get time from NTP server.");
    return false;
  } else {
    Serial.print("Current NTP time: ");
    Serial.println(ntpClient.getFormattedTime());
    return true;
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

  time_t currentTime = ntpClient.getEpochTime();
  struct tm *ti = localtime(&currentTime);
  
  struct tm startDate_tm = *ti;
  startDate_tm.tm_hour = 0;
  startDate_tm.tm_min = 0;
  startDate_tm.tm_sec = 0;
  
  time_t nextDayTime = currentTime + (24 * 3600);
  struct tm *nextDay_tm = localtime(&nextDayTime);
  struct tm endDate_tm = *nextDay_tm;
  endDate_tm.tm_hour = 23;
  endDate_tm.tm_min = 59;
  endDate_tm.tm_sec = 59;
  
  char startDateStr[9];
  char endDateStr[9];
  strftime(startDateStr, sizeof(startDateStr), "%Y%m%d", &startDate_tm);
  strftime(endDateStr, sizeof(endDateStr), "%Y%m%d", &endDate_tm);

  HTTPClient http;
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?station=";
  url += noaa_station_id;
  url += "&product=predictions&datum=MLLW&time_zone=lst_ldt&interval=hilo&units=english&application=DataAPI_Sample&format=json";
  url += "&begin_date="; url += startDateStr;
  url += "&end_date="; url += endDateStr;

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

    nextHighTideTime = 0; nextHighTideHeight = 0.0;
    nextLowTideTime = 0; nextLowTideHeight = 0.0;
    lastHighTideTime = 0;
    lastLowTideTime = 0;

    for (JsonObject prediction : predictions) {
        if (prediction.isNull()) continue;

        String type = prediction["type"].as<String>();
        String timeStr = prediction["t"].as<String>();
        float height = prediction["v"].as<float>();

        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &tm) == nullptr) {
             continue;
        }
        time_t tideUnixTime = mktime(&tm);

        if (tideUnixTime < currentTime) {
            if (type == "H") {
                if (tideUnixTime > lastHighTideTime) lastHighTideTime = tideUnixTime;
            } else if (type == "L") {
                if (tideUnixTime > lastLowTideTime) lastLowTideTime = tideUnixTime;
            }
        } else {
            if (type == "H" && nextHighTideTime == 0) {
                nextHighTideTime = tideUnixTime;
                nextHighTideHeight = height;
            } else if (type == "L" && nextLowTideTime == 0) {
                nextLowTideTime = tideUnixTime;
                nextLowTideHeight = height;
            }
        }
    }

    preferences.begin(PREF_NAMESPACE, false);
    preferences.putULong(PREF_NEXT_HIGH_TIDE_TIME, nextHighTideTime);
    preferences.putFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, nextHighTideHeight);
    preferences.putULong(PREF_NEXT_LOW_TIDE_TIME, nextLowTideTime);
    preferences.putFloat(PREF_NEXT_LOW_TIDE_HEIGHT, nextLowTideHeight);
    preferences.putULong(PREF_LAST_HIGH_TIDE_TIME, lastHighTideTime);
    preferences.putULong(PREF_LAST_LOW_TIDE_TIME, lastLowTideTime);
    preferences.end();
    
    Serial.println("Successfully downloaded and saved specific NOAA tide predictions.");
    return (nextHighTideTime > 0 || nextLowTideTime > 0);

  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}

// --- Simplified Servo Control Function (Unchanged) ---
void updateServoPosition() {
    Serial.println("--- Entering updateServoPosition (Simplified and Robust) ---");
    time_t currentTime = ntpClient.getEpochTime();
    
    time_t prevTideTime = 0;
    String prevTideType = "N/A";
    
    if (lastHighTideTime > lastLowTideTime) {
        prevTideTime = lastHighTideTime;
        prevTideType = "H";
    } else if (lastLowTideTime > 0) {
        prevTideTime = lastLowTideTime;
        prevTideType = "L";
    }

    time_t nextTideTime = 0;
    String nextTideType = "N/A";

    if (nextHighTideTime > 0 && (nextLowTideTime == 0 || nextHighTideTime < nextLowTideTime)) {
        nextTideTime = nextHighTideTime;
        nextTideType = "H";
    } else if (nextLowTideTime > 0) {
        nextTideTime = nextLowTideTime;
        nextTideType = "L";
    }
    
    if (prevTideTime == 0 || nextTideTime == 0) {
        Serial.println("Cannot find a valid past and future tide to map between. Aborting servo update.");
        digitalWrite(SERVO_POWER_PIN, HIGH);
        if (servoMotor.attached()) servoMotor.detach();
        Serial.println("--- Exiting updateServoPosition ---");
        return;
    }

    Serial.printf("Mapping current time (%ld) between past tide (%ld - %s) and next tide (%ld - %s).\n",
                  currentTime, prevTideTime, prevTideType.c_str(), nextTideTime, nextTideType.c_str());
    
    long totalCycleSeconds = nextTideTime - prevTideTime;
    long secondsIntoCycle = currentTime - prevTideTime;
    
    int newAngle = SERVO_LOW_TIDE_ANGLE;
    
    if (prevTideType == "H" && nextTideType == "L") {
        newAngle = map(secondsIntoCycle, 0, totalCycleSeconds, SERVO_HIGH_TIDE_ANGLE_1, SERVO_LOW_TIDE_ANGLE);
        newAngle = constrain(newAngle, SERVO_HIGH_TIDE_ANGLE_1, SERVO_LOW_TIDE_ANGLE);
        Serial.printf("  Falling tide (H->L). Mapping progress: %ld seconds over a %ld s cycle. Angle: %d\n",
                       secondsIntoCycle, totalCycleSeconds, newAngle);
    } else if (prevTideType == "L" && nextTideType == "H") {
        newAngle = map(secondsIntoCycle, 0, totalCycleSeconds, SERVO_LOW_TIDE_ANGLE, SERVO_HIGH_TIDE_ANGLE_2);
        newAngle = constrain(newAngle, SERVO_LOW_TIDE_ANGLE, SERVO_HIGH_TIDE_ANGLE_2);
        Serial.printf("  Rising tide (L->H). Mapping progress: %ld seconds over a %ld s cycle. Angle: %d\n",
                       secondsIntoCycle, totalCycleSeconds, newAngle);
    } else {
        Serial.printf("  Invalid tide transition sequence (%s -> %s). Setting default angle.\n", prevTideType.c_str(), nextTideType.c_str());
        newAngle = SERVO_LOW_TIDE_ANGLE;
    }

    newAngle = constrain(newAngle, 0, 180);
    
    Serial.printf("Servo position calculated. Writing angle: %d\n", newAngle);
    
    digitalWrite(SERVO_POWER_PIN, LOW);
    delay(50);
    servoMotor.attach(SERVO_SIGNAL_PIN);
    servoMotor.write(newAngle);
    delay(1000);
    servoMotor.detach();
    digitalWrite(SERVO_POWER_PIN, HIGH);
    
    Serial.println("--- Exiting updateServoPosition ---");
}

// --- Deep Sleep Function (Unchanged) ---
void deepSleep() {
  Serial.println("\n--- Entering deep sleep ---");
  
  time_t currentTime = ntpClient.getEpochTime();
  if (currentTime == 0) {
    Serial.println("NTP time failed. Sleeping for a short period to try again.");
    esp_sleep_enable_timer_wakeup(600ULL * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  } else {
    Serial.print("Current Time: ");
    printLocalTime(currentTime);
  }

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


// --- Arduino Setup and Loop (Unchanged) ---

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

  // New logic for WiFi setup
  bool wifiConnected = connectToWiFi();

  bool ntpSuccess = false;
  if (wifiConnected) {
    ntpSuccess = getTimeFromNTP();
  } else {
    Serial.println("WiFi connection failed. Cannot get new NTP time.");
  }
  
  if (ntpSuccess) {
    time_t currentTime = ntpClient.getEpochTime();
    struct tm *ti = localtime(&currentTime);
    int currentDay = ti->tm_mday;

    Serial.printf("Current day: %d, Last download day: %d\n", currentDay, lastDownloadDay);
    Serial.printf("Current NOAA Station ID: %s\n", noaa_station_id);

    bool needsNewData = false;
    
    if (currentDay != lastDownloadDay) {
        Serial.println("New day detected. Forcing new data download.");
        needsNewData = true;
    }
    
    if (strlen(noaa_station_id) == 0) {
        Serial.println("NOAA Station ID not set. Forcing new data download.");
        needsNewData = true;
    }

    time_t nextFutureTideTime = 0;
    if (nextHighTideTime > 0 && (nextLowTideTime == 0 || nextHighTideTime < nextLowTideTime)) {
        nextFutureTideTime = nextHighTideTime;
    } else if (nextLowTideTime > 0) {
        nextFutureTideTime = nextLowTideTime;
    }

    if (nextFutureTideTime > 0 && nextFutureTideTime < currentTime) {
        Serial.println("Closest future tide is in the past. Data is stale. Forcing new data download.");
        needsNewData = true;
    } else if (nextFutureTideTime == 0) {
        Serial.println("No future tide data found in NVS. Forcing new data download.");
        needsNewData = true;
    }

    if (needsNewData) {
      Serial.println("Attempting to download fresh tide data from NOAA...");
      if (getNOAATideData()) { 
        saveLastDownloadDay(currentDay);
        lastDownloadDay = currentDay;
        Serial.println("Tide data downloaded successfully and lastDownloadDay updated in NVS.");
      } else {
        Serial.println("Failed to download complete new tide data for the day. Servo might not function fully.");
      }
    } else {
      Serial.println("Using previously downloaded tide data.");
    }
  } else {
    Serial.println("NTP time update failed. Using last known NVS data for servo position.");
  }
  
  updateServoPosition();
delay(20000);
  deepSleep();
}

void loop() {
  // This part of the code will not be reached
}
