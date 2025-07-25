#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <HTTPClient.h>
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <NTPClient.h>   // https://github.com/arduino-libraries/NTPClient
#include <WiFiUdp.h>
#include <Preferences.h> // ESP32 built-in library for NVS
#include <ESP32Servo.h>  // Make sure you have this library installed

// Deep Sleep
#define uS_TO_S_FACTOR 1000000ULL // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP 1800ULL     // Time ESP32 will go to sleep (in seconds, 1 hour)

// GPIO Pins
#define RESET_BUTTON_PIN D9  // Connect button between D9 and GND
#define SERVO_POWER_PIN D7   // Controls the P-channel MOSFET gate
#define SERVO_SIGNAL_PIN D8  // PWM signal for the servo

// Servo parameters
#define SERVO_START_ANGLE 0
#define SERVO_MIDDLE_ANGLE 90
#define SERVO_END_ANGLE 180
// MAX_TIDE_CYCLE_HOURS is not directly used in the current servo angle calculation,
// but kept for reference if a more complex model is desired.
#define MAX_TIDE_CYCLE_HOURS 12.5 // Approximate hours between two consecutive high tides

// Preference Keys
const char* PREF_NAMESPACE = "tideClock";
const char* PREF_SSID = "wifi_ssid";
const char* PREF_NOAA_ID = "noaa_id";
const char* PREF_LAST_DOWNLOAD_DAY = "last_dl_day";
const char* PREF_NEXT_HIGH_TIDE_TIME = "next_ht_time";
const char* PREF_NEXT_LOW_TIDE_TIME = "next_lt_time";
const char* PREF_NEXT_HIGH_TIDE_HEIGHT = "next_ht_height";
const char* PREF_NEXT_LOW_TIDE_HEIGHT = "next_lt_height";

// WiFiManager and NVS objects
WiFiManager wm;
Preferences preferences;
WiFiUDP ntpUDP;
// NTP client configured for UTC-8 (Alaska Daylight Time)
NTPClient ntpClient(ntpUDP, "pool.ntp.org", -8 * 3600); 

// Global Sleep-proof variables (stored in NVS)
char wifi_ssid[60];
char noaa_station_id[10]; // <<< THIS IS THE CRITICAL GLOBAL DECLARATION
int lastDownloadDay = 0;
time_t nextHighTideTime = 0;
float nextHighTideHeight = 0.0;
time_t nextLowTideTime = 0;
float nextLowTideHeight = 0.0;

// Servo object
Servo servoMotor;

// Configuration parameters for WiFiManager
WiFiManagerParameter custom_noaa_station_id("stationid", "NOAA Station ID (e.g., 9455920)", "", 10);

// Function to save Wi-Fi SSID to NVS
void saveWifiConfiguration() {
  preferences.begin(PREF_NAMESPACE, false); // R/W mode
  preferences.putString(PREF_SSID, wifi_ssid);
  preferences.end();
  Serial.println("Wi-Fi SSID saved to NVS.");
}

// Function to save NOAA Station ID to NVS
void saveNoaaStationId(const char* id) {
  preferences.begin(PREF_NAMESPACE, false); // R/W mode
  preferences.putString(PREF_NOAA_ID, id);
  preferences.end();
  Serial.printf("NOAA Station ID '%s' saved to NVS.\n", id);
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
  preferences.getString(PREF_NOAA_ID, noaa_station_id, sizeof(noaa_station_id)); // Load NOAA ID
  lastDownloadDay = preferences.getInt(PREF_LAST_DOWNLOAD_DAY, 0); // Load lastDownloadDay
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
  wm.addParameter(&custom_noaa_station_id); // Add custom parameter for NOAA ID
  wm.setConfigPortalTimeout(180);

  Serial.println("Attempting WiFiManager autoConnect...");

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
    saveWifiConfiguration(); // Use the dedicated save function for SSID
    Serial.println("WiFi SSID updated and saved.");
  }

  // Check if a new NOAA Station ID was entered or updated via the portal
  if (strcmp(noaa_station_id, custom_noaa_station_id.getValue()) != 0 && strlen(custom_noaa_station_id.getValue()) > 0) {
    strcpy(noaa_station_id, custom_noaa_station_id.getValue());
    saveNoaaStationId(noaa_station_id); // Use dedicated save for NOAA ID
    Serial.println("NOAA Station ID updated and saved via portal.");
  } else if (strlen(noaa_station_id) == 0 && strlen(custom_noaa_station_id.getValue()) > 0) {
    // This handles the very first time where noaa_station_id is empty but a value was provided
    strcpy(noaa_station_id, custom_noaa_station_id.getValue());
    saveNoaaStationId(noaa_station_id);
    Serial.println("Initial NOAA Station ID set and saved via portal.");
  }
}

// Function to get tide data from NOAA API
bool getNOAATideData() {
  if (strlen(noaa_station_id) == 0) {
    Serial.println("NOAA Station ID not set. Cannot download tide data. Please configure via WiFiManager.");
    return false;
  }

  HTTPClient http;
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?date=today&station=";
  url += noaa_station_id; // Use the user-configured ID
  url += "&product=predictions&datum=MLLW&time_zone=lst_ldt&interval=hilo&units=english&application=DataAPI_Sample&format=json";

  Serial.print("NOAA API URL: ");
  Serial.println(url);

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
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

    // Reset the global 'next' tide variables before searching
    nextHighTideTime = 0;
    nextHighTideHeight = 0.0;
    nextLowTideTime = 0;
    nextLowTideHeight = 0.0;

    bool foundNextHigh = false;
    bool foundNextLow = false;

    // Iterate through all predictions to find the FIRST high and FIRST low tide
    // that are still in the future relative to currentUnixTime.
    for (JsonObject prediction : predictions) {
      String type = prediction["type"].as<String>();
      String timeStr = prediction["t"].as<String>(); // Format: YYYY-MM-DD HH:MM
      float height = prediction["v"].as<float>();

      struct tm tm;
      memset(&tm, 0, sizeof(tm));
      strptime(timeStr.c_str(), "%Y-%m-%d %H:%M", &tm);
      time_t tideUnixTime = mktime(&tm);

      // Only consider tides that are in the future or exactly now
      if (tideUnixTime >= currentUnixTime) {
        if (type == "H" && !foundNextHigh) {
          nextHighTideTime = tideUnixTime;
          nextHighTideHeight = height;
          foundNextHigh = true;
        } else if (type == "L" && !foundNextLow) {
          nextLowTideTime = tideUnixTime;
          nextLowTideHeight = height;
          foundNextLow = true;
        }

        // If we've found both the next future high and next future low, we can stop.
        if (foundNextHigh && foundNextLow) {
          break; // Exit the loop early
        }
      }
    }

    // Logging the results for debugging
    if (nextHighTideTime > 0) {
        Serial.print("Next High Tide (from NOAA data): "); Serial.print(asctime(localtime(&nextHighTideTime)));
        Serial.print("Height: "); Serial.println(nextHighTideHeight);
    } else {
        Serial.println("No future high tide found in NOAA data for today.");
    }
    if (nextLowTideTime > 0) {
        Serial.print("Next Low Tide (from NOAA data): "); Serial.print(asctime(localtime(&nextLowTideTime)));
        Serial.print("Height: "); Serial.println(nextLowTideHeight);
    } else {
        Serial.println("No future low tide found in NOAA data for today.");
    }

    // Report warning if not both were found (e.g., end of day, one passed already)
    if (!foundNextHigh || !foundNextLow) {
        Serial.println("Warning: Could not find both next high and low tides in future for current period.");
    }

    // Save the downloaded (and correctly identified future) tide data to NVS
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putULong(PREF_NEXT_HIGH_TIDE_TIME, nextHighTideTime);
    preferences.putFloat(PREF_NEXT_HIGH_TIDE_HEIGHT, nextHighTideHeight);
    preferences.putULong(PREF_NEXT_LOW_TIDE_TIME, nextLowTideTime);
    preferences.putFloat(PREF_NEXT_LOW_TIDE_HEIGHT, nextLowTideHeight);
    preferences.end(); // Commit the changes

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

// --- FUNCTION TO CONTROL THE SERVO ---
void updateServoPosition() {
    Serial.println("Updating servo position...");
    time_t currentTime = ntpClient.getEpochTime();
    
    // Check if we have at least one valid future tide prediction
    if (nextHighTideTime == 0 && nextLowTideTime == 0) {
        Serial.println("No future tide data to calculate servo position. Aborting.");
        return;
    }
    
    // Power on the servo by setting D7 LOW (P-channel MOSFET)
    digitalWrite(SERVO_POWER_PIN, LOW);
    delay(50); // Small delay for servo power to stabilize
    
    // Attach the servo and move it
    servoMotor.attach(SERVO_SIGNAL_PIN);
    
    // Determine the current tidal phase and base for calculation
    time_t referenceTideTime = 0;
    String referenceTideType = "";
    time_t nextFutureTideTime = 0; // The next actual tide event (high or low)
    String nextFutureTideType = "";

    // Find the *very next* future tide event, whether high or low.
    if (nextHighTideTime > currentTime && nextLowTideTime > currentTime) {
        // Both high and low are in the future, pick the sooner one
        if (nextHighTideTime < nextLowTideTime) {
            nextFutureTideTime = nextHighTideTime;
            nextFutureTideType = "High";
        } else {
            nextFutureTideTime = nextLowTideTime;
            nextFutureTideType = "Low";
        }
    } else if (nextHighTideTime > currentTime) {
        nextFutureTideTime = nextHighTideTime;
        nextFutureTideType = "High";
    } else if (nextLowTideTime > currentTime) {
        nextFutureTideTime = nextLowTideTime;
        nextFutureTideType = "Low";
    } else {
        Serial.println("All available tide predictions are in the past. Cannot update servo.");
        servoMotor.detach();
        digitalWrite(SERVO_POWER_PIN, HIGH);
        return;
    }

    // Now determine the reference point (previous tide) for the current segment
    // This is a simplified model:
    // High Tide (0 deg) -> Low Tide (90 deg) -> High Tide (180 deg, then reset to 0)

    // Scenario 1: We are moving from a LOW tide towards a HIGH tide
    // The previous tide (reference) was a low tide.
    if (nextFutureTideType == "High") {
        // We are approaching a High Tide. The "segment" started at the last Low Tide.
        // We need the time of the LAST low tide that occurred before currentTime.
        // For simplicity, using the `nextLowTideTime` if it's already in the past,
        // otherwise, this is a more complex look-back, which isn't available with current stored data.
        // For accurate tracking, the getNOAATideData should give us previous and next.
        // For now, let's assume `nextLowTideTime` is the 'start' if `nextHighTideTime` is the 'end'.
        referenceTideTime = nextLowTideTime; // This should be the most recent low tide time
        referenceTideType = "Low";

        // Check if our 'referenceTideTime' is actually in the past relative to current time
        // If it's not, or it's 0, it means we likely don't have enough data to determine this cycle.
        if (referenceTideTime == 0 || referenceTideTime >= currentTime) {
            Serial.println("Cannot find a valid past low tide reference for current high-tide approach.");
            // Fallback: If we can't determine the segment precisely, assume the pointer is at the next tide.
            // Or keep it simple for testing and only progress if the full segment can be determined.
            servoMotor.detach();
            digitalWrite(SERVO_POWER_PIN, HIGH);
            return;
        }

    }
    // Scenario 2: We are moving from a HIGH tide towards a LOW tide
    // The previous tide (reference) was a high tide.
    else if (nextFutureTideType == "Low") {
        // We are approaching a Low Tide. The "segment" started at the last High Tide.
        referenceTideTime = nextHighTideTime; // This should be the most recent high tide time
        referenceTideType = "High";

        if (referenceTideTime == 0 || referenceTideTime >= currentTime) {
            Serial.println("Cannot find a valid past high tide reference for current low-tide approach.");
            servoMotor.detach();
            digitalWrite(SERVO_POWER_PIN, HIGH);
            return;
        }
    }
    
    // If no valid reference was found for any reason
    if (referenceTideTime == 0) {
        Serial.println("No valid reference tide found for servo calculation. Skipping.");
        servoMotor.detach();
        digitalWrite(SERVO_POWER_PIN, HIGH);
        return;
    }

    long segmentDuration = abs(nextFutureTideTime - referenceTideTime); // Duration of the current tidal segment
    long elapsedTimeInSegment = currentTime - referenceTideTime;        // How far into the segment we are

    float progress = 0.0;
    if (segmentDuration > 0) {
        progress = (float)elapsedTimeInSegment / (float)segmentDuration;
    } else {
        Serial.println("Error: Segment duration is zero. Cannot calculate progress.");
        progress = 0.0;
    }
    
    int newAngle = 0;
    
    if (referenceTideType == "High") { // Current segment: High -> Low (0 to 90 degrees)
        newAngle = (int)(SERVO_START_ANGLE + progress * (SERVO_MIDDLE_ANGLE - SERVO_START_ANGLE));
    } else { // Current segment: Low -> High (90 to 180 degrees, then reset)
        newAngle = (int)(SERVO_MIDDLE_ANGLE + progress * (SERVO_END_ANGLE - SERVO_MIDDLE_ANGLE));
        // Reset to 0 if we've passed the "end of the dial" (another high tide)
        if (nextFutureTideType == "High" && currentTime >= nextFutureTideTime) {
            newAngle = SERVO_END_ANGLE; // Or 0, depending on desired visual reset behavior
            Serial.println("Reached / passed second High Tide. Servo conceptually resetting to 0.");
            // To visibly reset, you might write 0 then re-calculate on next cycle
        }
    }
    
    // Ensure the angle is within the valid range
    if (newAngle < SERVO_START_ANGLE) newAngle = SERVO_START_ANGLE;
    if (newAngle > SERVO_END_ANGLE) newAngle = SERVO_END_ANGLE;
    
    Serial.printf("Current Time: "); printLocalTime(currentTime);
    Serial.printf("Next Tide: %s at ", nextFutureTideType.c_str()); printLocalTime(nextFutureTideTime);
    Serial.printf("Reference Tide: %s at ", referenceTideType.c_str()); printLocalTime(referenceTideTime);
    Serial.printf("Segment Dur: %lds, Elapsed: %lds, Progress: %.2f, New Angle: %d\n",
                  segmentDuration, elapsedTimeInSegment, progress, newAngle);
    
    servoMotor.write(newAngle);
    
    delay(1000); // Wait for the servo to move
    
    // Power off the servo
    servoMotor.detach();
    digitalWrite(SERVO_POWER_PIN, HIGH); // Set D7 HIGH to turn off MOSFET
    Serial.println("Servo position updated and powered down.");
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

  // Re-evaluating the absolute next tide here for display purposes.
  // The 'nextHighTideTime' and 'nextLowTideTime' variables are loaded from NVS
  // and reflect the FIRST future high/low tide found during the last daily download.
  // We need to check if those loaded times are still in the future.
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
    // This is the message you were seeing. It means both nextHighTideTime and nextLowTideTime
    // (as stored from the last daily download) are now in the past.
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

  // Set up the GPIO for the servo power pin
  pinMode(SERVO_POWER_PIN, OUTPUT);
  digitalWrite(SERVO_POWER_PIN, HIGH); // Default to off (HIGH for P-channel MOSFET)

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed at boot. Clearing preferences and restarting.");
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

  // Only download new data if it's a new day or if NOAA Station ID is empty (first run after reset/flash)
  // This ensures a download happens initially if the ID isn't set yet.
  if (currentDay != lastDownloadDay || strlen(noaa_station_id) == 0) {
    Serial.println("New day detected or NOAA Station ID not yet set. Downloading fresh tide data from NOAA...");
    if (getNOAATideData()) {
      saveLastDownloadDay(currentDay); // Save the current day if download was successful
      lastDownloadDay = currentDay; // Also update in-memory variable
      Serial.println("Tide data downloaded successfully and lastDownloadDay updated in NVS.");
    } else {
      Serial.println("Failed to download new tide data for the day. Will use existing data or show 'N/A'.");
    }
  } else {
    Serial.println("Still same day and NOAA ID is set. Using previously downloaded tide data.");
  }

  // UPDATE SERVO POSITION
  updateServoPosition();

  deepSleep();
}

void loop() {
  // This part of the code will not be reached
}