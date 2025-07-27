#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ESP32Servo.h> // Make sure you have this library installed

// Deep Sleep Parameters
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 1800ULL // 1 hour (adjust as needed, e.g., 6 hours for more frequent updates: 6 * 3600ULL)

// GPIO Pins
#define RESET_BUTTON_PIN D9
#define SERVO_POWER_PIN D7   // Controls the P-channel MOSFET gate (LOW to turn on)
#define SERVO_SIGNAL_PIN D8 // PWM signal for the servo

// Servo Angle Parameters (Adjust these based on your servo's range and clock's mechanics)
// Assuming a clock with High (left), Low (middle), High (right)
#define SERVO_HIGH_TIDE_ANGLE_1 0    // Angle for a high tide mark (e.g., 0 degrees, far left)
#define SERVO_LOW_TIDE_ANGLE 90      // Angle for the low tide mark (e.g., 90 degrees, middle)
#define SERVO_HIGH_TIDE_ANGLE_2 180  // Angle for another high tide mark (e.g., 180 degrees, far right)


// NVS Preference Keys
const char* PREF_NAMESPACE = "tideClock";           // Namespace for all preferences
const char* PREF_SSID = "wifi_ssid";                // Key for WiFi SSID
const char* PREF_NOAA_ID = "noaa_id";               // Key for NOAA Station ID
const char* PREF_LAST_DOWNLOAD_DAY = "last_dl_day"; // Key for the day of last NOAA data download

// Storing specific tide prediction values in NVS (reverting to your successful strategy)
const char* PREF_NEXT_HIGH_TIDE_TIME = "next_ht_time";
const char* PREF_NEXT_HIGH_TIDE_HEIGHT = "next_ht_height";
const char* PREF_NEXT_LOW_TIDE_TIME = "next_lt_time";
const char* PREF_NEXT_LOW_TIDE_HEIGHT = "next_lt_height";


// WiFiManager and NVS Objects
WiFiManager wm;
Preferences preferences;
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org", -8 * 3600); // NTP client, -8 * 3600 for AKDT (UTC-8)

// Global Configuration Variables (Loaded from NVS or set by WiFiManager/NTP)
char wifi_ssid[60];          // Stores connected WiFi SSID
char noaa_station_id[10];    // Stores NOAA Station ID (e.g., "9455920")
int lastDownloadDay = 0;     // Stores the day of the month when tide data was last downloaded

// Global variables for the *next* specific tide events (populated from NOAA and saved to NVS)
time_t nextHighTideTime = 0;
float nextHighTideHeight = 0.0;
time_t nextLowTideTime = 0;
float nextLowTideHeight = 0.0;

// Servo Object
Servo servoMotor;

// WiFiManager Custom Parameter
WiFiManagerParameter custom_noaa_station_id("stationid", "NOAA Station ID (e.g., 9455920)", "", 10);


// --- NVS Helper Functions ---

void saveWifiConfiguration() {
  preferences.begin(PREF_NAMESPACE, false); // Read-write mode
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

// Function to load all configuration and saved tide data from NVS
void loadConfiguration() {
  preferences.begin(PREF_NAMESPACE, true); // Read-only mode
  preferences.getString(PREF_SSID, wifi_ssid, sizeof(wifi_ssid));
  preferences.getString(PREF_NOAA_ID, noaa_station_id, sizeof(noaa_station_id));
  lastDownloadDay = preferences.getInt(PREF_LAST_DOWNLOAD_DAY, 0);

  // Load specific tide prediction values
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


// --- WiFiManager Configuration ---

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
  wm.setConfigPortalTimeout(180); // 3 minutes timeout for configuration

  Serial.println("Attempting WiFiManager autoConnect...");
  if (!wm.autoConnect("TideClockSetupAP", "password")) {
    Serial.println("Failed to connect and hit timeout. Restarting...");
    ESP.restart(); // Restart if WiFi connection or setup fails
  }
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Save WiFi SSID if it changed
  if (strcmp(wifi_ssid, WiFi.SSID().c_str()) != 0) {
    strcpy(wifi_ssid, WiFi.SSID().c_str());
    saveWifiConfiguration();
    Serial.println("WiFi SSID updated and saved.");
  }
  // Save NOAA Station ID if it changed or was newly set
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


// --- Time and NOAA Data Functions ---

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

// Helper function to print time in human-readable format
void printLocalTime(time_t epochTime) {
    struct tm *timeinfo;
    timeinfo = localtime(&epochTime);
    Serial.print(asctime(timeinfo));
}

// Function to get tide data from NOAA API and store NEXT high/low tides
bool getNOAATideData() {
  if (strlen(noaa_station_id) == 0) {
    Serial.println("NOAA Station ID not set. Cannot download tide data. Please configure via WiFiManager.");
    return false;
  }

  HTTPClient http;
  // Request data for today + next day to ensure we capture future tides even if close to midnight
  // It's safer to get a wider range and filter.
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?date=today&range=48&station="; // 48 hours from start of 'today'
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

    // Use a larger capacity if your JSON payload is big. 
    // For 4 predictions, 16384 bytes should be more than enough, but 4000 might also work.
    // Let's stick with 16384 to be safe, as memory isn't the issue here, but repeated allocation was.
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
    time_t currentTime = ntpClient.getEpochTime(); // Get current time once

    // Find the *first* future high and low tides
    for (JsonObject prediction : predictions) {
        if (prediction.isNull()) continue; // Skip malformed entries

        String type = prediction["type"].as<String>();
        String timeStr = prediction["t"].as<String>();
        float height = prediction["v"].as<float>();

        struct tm tm;
        memset(&tm, 0, sizeof(tm)); // Clear struct before parsing
        if (strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &tm) == nullptr) {
             Serial.printf("  Skipping prediction: Failed to parse time string '%s'.\n", timeStr.c_str());
             continue;
        }
        time_t tideUnixTime = mktime(&tm);

        // Only consider tides in the future or very close to current time (e.g., within a minute or two)
        if (tideUnixTime >= currentTime - 120) { // Allow for a small buffer if current time is slightly after prediction
            if (type == "H" && !foundNextHigh) {
                nextHighTideTime = tideUnixTime;
                nextHighTideHeight = height;
                foundNextHigh = true;
            } else if (type == "L" && !foundNextLow) {
                nextLowTideTime = tideUnixTime;
                nextLowTideHeight = height;
                foundNextLow = true;
            }
            // If we found both, we can stop iterating
            if (foundNextHigh && foundNextLow) {
                break;
            }
        }
    }

    // Log the results found
    if (nextHighTideTime > 0) {
        Serial.print("Identified Next High Tide: "); Serial.print(asctime(localtime(&nextHighTideTime)));
        Serial.print("  Height: "); Serial.println(nextHighTideHeight);
    } else {
        Serial.println("No future high tide identified in fetched data.");
    }
    if (nextLowTideTime > 0) {
        Serial.print("Identified Next Low Tide: "); Serial.print(asctime(localtime(&nextLowTideTime)));
        Serial.print("  Height: "); Serial.println(nextLowTideHeight);
    } else {
        Serial.println("No future low tide identified in fetched data.");
    }

    // Save the identified next tide data to NVS
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putULong(PREF_NEXT_HIGH_TIDE_TIME, nextHighTideTime);
    preferences.putFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, nextHighTideHeight);
    preferences.putULong(PREF_NEXT_LOW_TIDE_TIME, nextLowTideTime);
    preferences.putFloat(PREF_NEXT_LOW_TIDE_HEIGHT, nextLowTideHeight);
    preferences.end();
    
    Serial.println("Successfully downloaded and saved specific NOAA tide predictions.");
    http.end();
    return true;

  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}

// --- Servo Control Function ---

void updateServoPosition() {
    Serial.println("--- Entering updateServoPosition ---");
    time_t currentTime = ntpClient.getEpochTime();
    
    // Declare newAngle here so it's accessible throughout the function
    int newAngle = SERVO_LOW_TIDE_ANGLE; // Initialize to a default safe position (e.g., Low Tide)
    
    // Check if we have at least one valid future tide prediction loaded from NVS
    if (nextHighTideTime == 0 && nextLowTideTime == 0) {
        Serial.println("No future tide data available (nextHighTideTime and nextLowTideTime are both 0). Aborting servo update.");
        digitalWrite(SERVO_POWER_PIN, HIGH); // Ensure servo power is off
        if (servoMotor.attached()) {
            servoMotor.detach();
        }
        return; 
    }

    time_t prevTideTime = 0;
    String prevTideType = ""; // "H" for High, "L" for Low
    time_t actualNextTideTime = 0;
    String actualNextTideType = "";

    // Determine the most recent past tide and the next upcoming tide based on current time
    // This logic needs to be robust to handle various scenarios (e.g., current time is before first prediction, or after all predictions)

    // Scenario 1: Both recorded high and low tides are in the future
    if (nextHighTideTime > currentTime && nextLowTideTime > currentTime) {
        Serial.println("Current time is before both next high and next low tides from NVS.");
        // This implies the current time is before the first *recorded* tide.
        // We'll calculate based on the first *closest* future tide.
        // This means the "previous" point for interpolation is effectively unknown or outside our data range.
        // For simplicity, if we're before any recorded data, we can set it to the start of the first upcoming cycle.
        
        // Find which one is the absolute next event
        if (nextHighTideTime < nextLowTideTime) {
            actualNextTideTime = nextHighTideTime;
            actualNextTideType = "H";
            // If the next event is High, the previous *relevant* event would have been a Low.
            // If nextLowTideTime is also in the future, it cannot be the previous.
            // This is an edge case: if we just booted and the first prediction is for hours from now.
            // We can only set it to the start of its intended range.
            newAngle = SERVO_HIGH_TIDE_ANGLE_1; // Default to a 'high' position
            Serial.println("Setting angle to SERVO_HIGH_TIDE_ANGLE_1 as current time is before all predictions.");

        } else {
            actualNextTideTime = nextLowTideTime;
            actualNextTideType = "L";
            newAngle = SERVO_LOW_TIDE_ANGLE; // Default to a 'low' position
            Serial.println("Setting angle to SERVO_LOW_TIDE_ANGLE as current time is before all predictions.");
        }
        
        // If we are in this scenario, we cannot meaningfully interpolate as we don't have a 'previous'
        // point within the downloaded data that is actually in the past.
        // So, we set a default angle and exit the interpolation part.
        digitalWrite(SERVO_POWER_PIN, LOW); // Power on
        delay(50);
        servoMotor.attach(SERVO_SIGNAL_PIN);
        servoMotor.write(newAngle);
        delay(1000);
        servoMotor.detach();
        digitalWrite(SERVO_POWER_PIN, HIGH); // Power off
        Serial.println("--- Exiting updateServoPosition (no valid past tide for interpolation) ---");
        return; // Exit as interpolation isn't possible
    } 
    // Scenario 2: Current time is past high tide, but before low tide (falling tide H -> L)
    else if (currentTime >= nextHighTideTime && (nextLowTideTime == 0 || currentTime < nextLowTideTime)) {
        prevTideTime = nextHighTideTime;
        prevTideType = "H";
        actualNextTideTime = nextLowTideTime;
        actualNextTideType = "L";
        Serial.printf("Current time past High (%ld), before Low (%ld). Falling tide (H->L).\n", prevTideTime, actualNextTideTime);
    } 
    // Scenario 3: Current time is past low tide, but before high tide (rising tide L -> H)
    else if (currentTime >= nextLowTideTime && (nextHighTideTime == 0 || currentTime < nextHighTideTime)) {
        prevTideTime = nextLowTideTime;
        prevTideType = "L";
        actualNextTideTime = nextHighTideTime;
        actualNextTideType = "H";
        Serial.printf("Current time past Low (%ld), before High (%ld). Rising tide (L->H).\n", prevTideTime, actualNextTideTime);
    } 
    // Scenario 4: All available NVS tide predictions are in the past.
    else {
        // This typically happens if the data from NOAA is old (e.g., current time is next day, but data is from yesterday).
        // This means the daily data refresh *should* have kicked in, but didn't, or failed.
        Serial.println("All available NVS tide predictions are in the past. Cannot calculate position.");
        digitalWrite(SERVO_POWER_PIN, HIGH); // Ensure servo power is off
        if (servoMotor.attached()) servoMotor.detach();
        Serial.println("--- Exiting updateServoPosition (old data) ---");
        return; // Exit as we can't determine current tide cycle.
    }

    // Double-check for valid tide points for interpolation before proceeding
    if (prevTideTime == 0 || actualNextTideTime == 0 || actualNextTideTime <= prevTideTime) {
         Serial.printf("ERROR: Invalid tide points for interpolation. prevTideTime: %ld, actualNextTideTime: %ld. Aborting.\n", prevTideTime, actualNextTideTime);
         digitalWrite(SERVO_POWER_PIN, HIGH);
         if (servoMotor.attached()) servoMotor.detach();
         Serial.println("--- Exiting updateServoPosition (invalid interpolation points) ---");
         return;
    }

    // Power on and attach servo ONLY if we have valid data and intend to move it
    Serial.println("Powering on servo...");
    digitalWrite(SERVO_POWER_PIN, LOW); // Set D7 LOW to turn on P-channel MOSFET (power to servo)
    delay(50); // Small delay for servo power to stabilize
    
    Serial.println("Attaching servo...");
    servoMotor.attach(SERVO_SIGNAL_PIN);
    
    long cycleDuration = actualNextTideTime - prevTideTime;
    long timeIntoCycle = currentTime - prevTideTime;

    Serial.printf("Servo calc: prevTideType='%s' (time: %ld), actualNextTideType='%s' (time: %ld), currentTime=%ld\n",
                  prevTideType.c_str(), prevTideTime, actualNextTideType.c_str(), actualNextTideTime, currentTime);
    
    if (cycleDuration <= 0) {
        Serial.println("Error: Cycle duration is zero or negative. Setting default angle to LOW.");
        newAngle = SERVO_LOW_TIDE_ANGLE;
    } else {
        float progress = (float)timeIntoCycle / cycleDuration;
        // Clamp progress to ensure it's within 0.0 to 1.0
        if (progress < 0.0) progress = 0.0;
        if (progress > 1.0) progress = 1.0;

        // Interpolate angle based on progress through the cycle
        if (prevTideType == "H" && actualNextTideType == "L") {
            newAngle = SERVO_HIGH_TIDE_ANGLE_1 + (int)(progress * (SERVO_LOW_TIDE_ANGLE - SERVO_HIGH_TIDE_ANGLE_1));
            Serial.printf("  Falling tide (H->L) calculated progress: %.2f, raw angle: %d (0-90)\n", progress, newAngle);
        } else if (prevTideType == "L" && actualNextTideType == "H") {
            newAngle = SERVO_LOW_TIDE_ANGLE + (int)(progress * (SERVO_HIGH_TIDE_ANGLE_2 - SERVO_LOW_TIDE_ANGLE));
            Serial.printf("  Rising tide (L->H) calculated progress: %.2f, raw angle: %d (90-180)\n", progress, newAngle);
        } else {
            // This 'else' condition should ideally not be hit with robust prev/next determination.
            Serial.printf("Warning: Unexpected tide transition types (%s -> %s). Setting default angle to LOW.\n", 
                          prevTideType.c_str(), actualNextTideType.c_str());
            newAngle = SERVO_LOW_TIDE_ANGLE;
        }
    }
    
    // Clamp angle to valid servo range (0-180)
    if (newAngle < 0) newAngle = 0;
    if (newAngle > 180) newAngle = 180;
    
    Serial.printf("Servo position calculated. Writing angle: %d\n", newAngle);
    servoMotor.write(newAngle);
    
    delay(1000); // Wait for the servo to physically move to the new position
    
    // Always power off and detach after moving
    Serial.println("Powering off servo...");
    servoMotor.detach();
    digitalWrite(SERVO_POWER_PIN, HIGH);
    Serial.println("--- Exiting updateServoPosition ---");
}


// --- Deep Sleep Function ---

void deepSleep() {
  Serial.println("\n--- Entering deep sleep ---");

  time_t currentTime = ntpClient.getEpochTime();
  Serial.print("Current Time: ");
  printLocalTime(currentTime);

  Serial.println("\n--- Next Tide Info (from NVS) ---");
  time_t nextFutureTideTime = 0;
  String nextFutureTideType = "N/A";
  float nextFutureTideHeight = 0.0;

  // Determine which is the *soonest* next tide in the future
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


// --- Arduino Setup and Loop ---

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting Tide Clock...");

  pinMode(SERVO_POWER_PIN, OUTPUT);
  digitalWrite(SERVO_POWER_PIN, HIGH); // Default to off (HIGH for P-channel MOSFET)

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed at boot. Clearing all preferences and restarting.");
    preferences.begin(PREF_NAMESPACE, false);
    preferences.clear(); // Clear all keys in the namespace
    preferences.end();
    wm.resetSettings(); // Also clear WiFiManager settings
    delay(1000);
    ESP.restart();
  }

  loadConfiguration(); // Load all saved data from NVS

  setupWiFiManager(); // Connect to WiFi and handle configuration portal if needed

  getTimeFromNTP(); // Update current time via NTP

  time_t currentTime = ntpClient.getEpochTime();
  struct tm *ti = localtime(&currentTime);
  int currentDay = ti->tm_mday;

  Serial.printf("Current day: %d, Last download day: %d\n", currentDay, lastDownloadDay);
  Serial.printf("Current NOAA Station ID: %s\n", noaa_station_id);

  // Check if we need to download new tide data
  // Conditions: new day, NOAA ID not set, OR no valid future tide data in NVS
  bool needsNewData = false;
  if (currentDay != lastDownloadDay) {
      Serial.println("New day detected.");
      needsNewData = true;
  }
  if (strlen(noaa_station_id) == 0) {
      Serial.println("NOAA Station ID not set.");
      needsNewData = true;
  }
  // Check if current loaded tide data is still valid (in the future)
  if (nextHighTideTime == 0 && nextLowTideTime == 0) {
      Serial.println("No future tide data loaded from NVS (both times are 0).");
      needsNewData = true;
  } else if (nextHighTideTime > 0 && nextHighTideTime < currentTime) {
      Serial.println("Next High Tide from NVS is in the past. Need fresh data.");
      needsNewData = true;
  } else if (nextLowTideTime > 0 && nextLowTideTime < currentTime) {
      Serial.println("Next Low Tide from NVS is in the past. Need fresh data.");
      needsNewData = true;
  }
  // If both are in the past or one is, it's safer to refresh.

  if (needsNewData) {
    Serial.println("Attempting to download fresh tide data from NOAA...");
    if (getNOAATideData()) { // This function also saves the data to NVS
      saveLastDownloadDay(currentDay); // Only update day if download was successful
      lastDownloadDay = currentDay;
      Serial.println("Tide data downloaded successfully and lastDownloadDay updated in NVS.");
    } else {
      Serial.println("Failed to download new tide data for the day. Will attempt to use existing data if any, otherwise servo won't update.");
    }
  } else {
    Serial.println("Still same day, NOAA ID is set, and loaded tide data is current. Using previously downloaded tide data.");
  }

  updateServoPosition(); // Calculate and set servo position
delay(20000);
  deepSleep(); // Enter deep sleep
}

void loop() {
  // This part of the code will not be reached as the ESP32 enters deep sleep.
}