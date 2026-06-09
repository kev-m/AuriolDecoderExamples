#include <Arduino.h>
#include <AuriolDecoder.h>

// Mandatory: Required by the library
#define RF_DATA_PIN 2

// Optional: Project Specific
#define RF_POWER_PIN 3

// Callback invoked when sensor packet is decoded
void onSensorPacket(uint16_t sensor_id, uint8_t channel, float temp, uint8_t humidity, bool battery_ok) {
  Serial.printf("Sensor 0x%02X: Channel: %d, %.1f°C, %d%% Humidity, Battery: %s\n",
    sensor_id, channel, temp, (int)humidity, battery_ok ? "OK" : "Low");
}

void setup() {
  Serial.begin(115200);
  
  // Initialize RF power control (module-specific)
  pinMode(RF_POWER_PIN, OUTPUT);
  digitalWrite(RF_POWER_PIN, LOW);  // Enable RF module
  
  // Initialize decoder
  auriolDecoderSetup(RF_DATA_PIN, onSensorPacket);
}

void loop() {
  // Process RF data - call regularly in main loop
  auriolDecoderProcess();
  yield();  // Yield to ESP8266 core tasks
}