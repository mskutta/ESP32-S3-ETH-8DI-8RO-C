// Main program for ESP32-S3-ETH-8DI-8RO-C project
// Implements OSC TCP control for 8 relays via Ethernet, with RGB LED activity indicator

#include <Arduino.h>
#include "WS_ETH.h"
#include "WS_GPIO.h"
#include <SPI.h>
#include <ArduinoOSCETH.h>
#include "WS_Relay.h"

// Trigger timers
unsigned long triggerTimers[8] = {0};
bool triggerActive[8] = {false};

// OSC callback function for relays
void relayCallback(const OscMessage& m) {
  // Get the address
  String addr = m.address();
  // Find the last / to get the relay number
  int lastSlash = addr.lastIndexOf('/');
  String numStr = addr.substring(lastSlash + 1);
  int relayNum = numStr.toInt();
  int relayIndex = relayNum - 1;

  if (relayIndex < 0 || relayIndex >= 8) return;

  if (m.isInt32(0)) {
    int value = m.arg<int>(0);

    if (value == 0) {
      // Off
      Relay_Closs(relayNum);
      RGB_Light(255, 0, 0); // Red
      delay(200);
      RGB_Light(0, 0, 0);   // Off
      Serial.printf("Relay %d OFF\n", relayIndex + 1);
    } else if (value == 1) {
      // On
      Relay_Open(relayNum);
      RGB_Light(0, 255, 0); // Green
      delay(200);
      RGB_Light(0, 0, 0);   // Off
      Serial.printf("Relay %d ON\n", relayIndex + 1);
    } else if (value == 2) {
      // Trigger: on for 1 second
      Relay_Open(relayNum);
      triggerActive[relayIndex] = true;
      triggerTimers[relayIndex] = millis();
      RGB_Light(0, 0, 255); // Blue
      delay(200);
      RGB_Light(0, 0, 0);   // Off
      Serial.printf("Relay %d TRIGGER\n", relayIndex + 1);
    }
  }
}

void setup() {
  delay(1000);  // Wait for USB enumeration
  Serial.begin(115200);
  
  Serial.println("\n\n=== ESP32-S3 Relay Control Starting ===");

  GPIO_Init();  // Initialize GPIO for RGB and Buzzer
  I2C_Init();   // Initialize I2C for expander

  // Show startup status with RGB LED
  Serial.println("Initializing RGB LED...");
  RGB_Light(255, 0, 0); // Red
  delay(200);

  // Initialize relays as outputs, default off (HIGH)
  Serial.println("Initializing relays...");
  Relay_Init();

  // test relays on startup
  Serial.println("Testing relays...");
  for (int i = 1; i <= 8; i++) {
    Relay_Open(i);
    delay(100);
    Relay_Closs(i);
  }
  Serial.println("Relay test complete");

  RGB_Light(0, 0, 255); // Blue
  delay(200);

  // Build hostname from board MAC ID, e.g. relay8-1A2B3C
  Serial.println("Building hostname...");
  uint64_t chipid = ESP.getEfuseMac();
  uint32_t id = (uint32_t)(chipid & 0xFFFFFF);
  char hostname[24];
  snprintf(hostname, sizeof(hostname), "relay8-%06X", id);
  Serial.print("Hostname: ");
  Serial.println(hostname);

  // Initialize native ESP32 Ethernet using the Waveshare wiring.
  Serial.println("Initializing Ethernet...");
  ETH_SetHostname(hostname);
  ETH_Init();
  
  // Wait for Ethernet to get IP
  Serial.println("Waiting for Ethernet IP...");
  int timeout = 0;
  while (!ETH_Connected() && timeout < 100) {
    ETH_Loop();
    delay(100);
    timeout++;
  }
  
  if (ETH_Connected()) {
    Serial.println("Ethernet ready");
    Serial.print("IP address: ");
    Serial.println(ETH_LocalIP());
  } else {
    Serial.println("WARNING: Ethernet connection timeout - continuing anyway");
  }

  // Subscribe to OSC messages on port 53000
  OscEther.subscribe(53000, "/relay/*", relayCallback);

  Serial.println("OSC TCP server started on port 53000");

  RGB_Light(0, 255, 0); // Green
  delay(200);
}

void loop() {
  // Update OSC to receive messages
  OscEther.update();

  // Update Ethernet status
  ETH_Loop();

  // Handle trigger timers
  unsigned long currentMillis = millis();
  for (int i = 0; i < 8; i++) {
    if (triggerActive[i] && (currentMillis - triggerTimers[i] >= 1000)) {
      Relay_Closs(i + 1); // Turn off
      triggerActive[i] = false;
      Serial.printf("Relay %d trigger off\n", i + 1);
    }
  }
}
