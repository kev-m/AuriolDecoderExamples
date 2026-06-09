/**
 * Project: ESP-01S 433 MHz Auriol Temperature / Temperature + Humidity sensor decoder.
 * Version: 1.0
 * Target:  ESP8266 (ESP-01S)
 * * Pinout Allocation:
 * - GPIO0 (IO0) :
 * - GPIO1 (TXD0): Available for standard Serial Debugging (One-way telemetry output to PC)
 * - GPIO2 (IO2) : RF data in
 * - GPIO3 (RXD0): RF chip Power
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <AuriolDecoder.h>

////////////////////////////////////////////////////////////
// CONFIGURATION
////////////////////////////////////////////////////////////
#define WIFI_HOSTNAME "auriol-gw"

// Pin Configuration
#define RF_DATA_PIN 2  // GPIO2 for the data stream
#define RX_POWER_PIN 3 // GPIO3 enables the receiver transistor

// Common: Wifi, UDP, OTA, etc
// Options: SERIAL_DEBUG, UDP_HEARTBEAT_PORT (requires DEB_VER), LED_PIN
// Requires: void disableRF();
void disableRF();
#define DEB_VER 70
#define UDP_HEARTBEAT_PORT 4242
#include "common.h"

// Instantiations
WiFiClient espClient;
unsigned long last_reconnect_attempt = 0;
unsigned long last_stats_report = 0; // Timer tracking for 5-second UDP dump

void enableRF()
{
    digitalWrite(RX_POWER_PIN, LOW);
}

void disableRF()
{
    digitalWrite(RX_POWER_PIN, HIGH);
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
// Callback invoked when sensor packet is decoded
void onSensorPacket(uint16_t sensor_id, uint8_t channel, float temperature_c, uint8_t humidity_pct, bool battery_ok)
{
    sendDebug(13);
    char jsonBuffer[1024];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
        "{"
        "\"type\":\"Auriol\","
        "\"sensor_id\":\"%04X\","
        "\"channel\":%u,"
        "\"temperature_c\":%.1f,"
        "\"humidity_pct\":%u,"
        "\"battery\":\"%s\""
        "}",
        sensor_id, 
        (unsigned int)channel, 
        temperature_c, 
        humidity_pct, 
        battery_ok ? "OK" : "Low"
    );

    sendUdp(jsonBuffer);
}
/////////////////////////////////////////////////////////////////////////////
void setup()
{
    // 1. Initialize power rails for the 433MHz receiver stage
    pinMode(RX_POWER_PIN, OUTPUT);
    enableRF();

    // 2. Attach the Manchester edge processing ISR
    // Initialize decoder
    auriolDecoderSetup(RF_DATA_PIN, onSensorPacket);

    // 3. Network initialization
    wifi_enable();

    // Allow hardware crystal and oscillator stages to settle
    delay(100);
}

void loop()
{
    if (wifi_enabled)
    {
        ArduinoOTA.handle();
    }

    // Process RF data - call regularly in main loop
    auriolDecoderProcess();

    // Yield execution to protect core ESP8266 Wi-Fi context layers
    yield();
}