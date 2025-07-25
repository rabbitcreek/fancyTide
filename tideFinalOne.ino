#include <Arduino.h> // Standard Arduino core functions
#include <ESP32Servo.h> // Library for controlling servos on ESP32

// GPIO Pins
#define SERVO_POWER_PIN D7  // Controls the P-channel MOSFET gate
#define SERVO_SIGNAL_PIN D8 // PWM signal for the servo

// Deep Sleep Parameters
#define uS_TO_S_FACTOR 1000000ULL // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP 60ULL       // Time ESP32 will go to sleep (in seconds, 1 minute)

// Servo object
Servo testServo;

void setup() {
  Serial.begin(115200);
  delay(100); // Give serial a moment to initialize
  Serial.println("\n--- Servo Test Program Starting ---");

  // Configure SERVO_POWER_PIN as an output
  pinMode(SERVO_POWER_PIN, OUTPUT);

  // Initially set SERVO_POWER_PIN HIGH to keep the P-channel MOSFET OFF (servo unpowered)
  // A P-channel MOSFET turns ON when its Gate is LOW relative to its Source.
  // When ESP32 GPIO is HIGH, it pulls the gate towards VCC, turning the MOSFET OFF.
  digitalWrite(SERVO_POWER_PIN, HIGH);
  Serial.println("Servo power pin (D7) set HIGH (servo OFF).");
  delay(500); // Short delay before powering on

  Serial.println("Powering on servo (D7 LOW)...");
  // Turn on power to the servo by setting D7 LOW (turns on P-channel MOSFET)
  digitalWrite(SERVO_POWER_PIN, LOW);
  delay(100); // Small delay for servo power to stabilize

  // Attach the servo to the signal pin
  testServo.attach(SERVO_SIGNAL_PIN);
  Serial.printf("Servo attached to D%d.\n", SERVO_SIGNAL_PIN);

  Serial.println("Sweeping servo from 0 to 180 degrees...");
  // Sweep servo from 0 to 180 degrees
  for (int pos = 0; pos <= 180; pos += 1) { // goes from 0 degrees to 180 degrees
    testServo.write(pos);                  // tell servo to go to position in variable 'pos'
    delay(15);                             // waits 15ms for the servo to reach the position
  }

  Serial.println("Sweeping servo from 180 to 0 degrees...");
  for (int pos = 180; pos >= 0; pos -= 1) { // goes from 180 degrees to 0 degrees
    testServo.write(pos);                   // tell servo to go to position in variable 'pos'
    delay(15);                              // waits 15ms for the servo to reach the position
  }

  delay(500); // Wait a bit after the sweep

  // Detach the servo to release the PWM signal and save power
  testServo.detach();
  Serial.println("Servo detached from D8.");

  // Turn off power to the servo by setting D7 HIGH (turns off P-channel MOSFET)
  digitalWrite(SERVO_POWER_PIN, HIGH);
  Serial.println("Servo power pin (D7) set HIGH (servo OFF).");
  delay(100); // Ensure MOSFET has time to switch off

  Serial.printf("Entering deep sleep for %llu seconds...\n", TIME_TO_SLEEP);
  // Configure ESP32 to wake up after TIME_TO_SLEEP seconds
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  // Enter deep sleep
  esp_deep_sleep_start();
}

void loop() {
  // This loop will not be executed as the ESP32 enters deep sleep from setup()
  // and restarts from setup() upon waking up.
}