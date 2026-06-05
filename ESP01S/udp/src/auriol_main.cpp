/**
 * Project: ESP-01S Direct PWM / Hardware Timer DCF77 Transmitter
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
#include <ArduinoJson.h>

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
// New Pulse-Distance Timing Targets (in microseconds)
#define CYCLE_SHORT_EXPECTED 2400
#define CYCLE_LONG_EXPECTED 4400
#define PDM_TOLERANCE 400

// Volatile state storage for the PDM engine
volatile uint64_t rxBuffer = 0;
volatile uint8_t bitCount = 0;
volatile bool packetReady = false;

// Instrumentation tracking
volatile uint32_t metric_total_edges = 0;
volatile uint32_t metric_bad_timings = 0;
volatile uint8_t metric_max_bits = 0;

IRAM_ATTR void rf_edge_isr()
{
    metric_total_edges++;
    static unsigned long lastRisingTime = 0;

    unsigned long now = micros();
    unsigned long duration = now - lastRisingTime;
    lastRisingTime = now;

    // Glitch protector: ignore edges that double-trigger too fast
    if (duration < 1000)
        return;

    // Evaluate the total cycle length from the previous rising edge to this one
    bool isShort = (duration > (CYCLE_SHORT_EXPECTED - PDM_TOLERANCE) && duration < (CYCLE_SHORT_EXPECTED + PDM_TOLERANCE));
    bool isLong = (duration > (CYCLE_LONG_EXPECTED - PDM_TOLERANCE) && duration < (CYCLE_LONG_EXPECTED + PDM_TOLERANCE));

    // A packet preamble or sync gap typically presents as a huge duration (>8000us)
    if (!isShort && !isLong)
    {
        // If we hit a sync mark and have collected a clean frame, lock it down.
        if (bitCount == 32)
        {
            packetReady = true;
        }
        else
        {
            if (bitCount > metric_max_bits)
                metric_max_bits = bitCount;
            bitCount = 0;
            rxBuffer = 0;
        }
        metric_bad_timings++;
        return;
    }
    // Shift data bits only if the buffer hasn't been locked by the main loop execution
    // Inside the bit-shifting block of rf_edge_isr():
    if (!packetReady)
    {
        uint8_t sampledBit = isShort ? 1 : 0;

        rxBuffer = (rxBuffer << 1) | sampledBit;
        bitCount++;

        // Change from 40 to 32
        if (bitCount >= 32)
        {
            packetReady = true;
        }
    }
}

void setup()
{
    // 1. Initialize power rails for the 433MHz receiver stage
    pinMode(RX_POWER_PIN, OUTPUT);
    enableRF();

    // 2. Attach the Manchester edge processing ISR
    pinMode(RF_DATA_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(RF_DATA_PIN), rf_edge_isr, RISING);

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

    unsigned long currentMillis = millis();

    // Periodic 5-second instrumentation diagnostics report
    if (currentMillis - last_stats_report >= 5000)
    {
        // Prepare for payload
        sendDebug(34);

        last_stats_report = currentMillis;

        // Perform an atomic read-and-clear of volatile metric registers
        noInterrupts();
        uint32_t totalEdges = metric_total_edges;
        uint32_t badTimings = metric_bad_timings;
        uint8_t maxBits = metric_max_bits;

        // Reset diagnostic metrics for the next tracking window
        metric_total_edges = 0;
        metric_bad_timings = 0;
        metric_max_bits = 0;
        interrupts();

        // Package up diagnostics payload
        JsonDocument statsDoc;
        statsDoc["type"] = "rfrx_metrics";
        statsDoc["total_edges"] = totalEdges;
        statsDoc["bad_timings"] = badTimings;
        statsDoc["max_bit_depth"] = maxBits;

        char statsBuffer[256];
        serializeJson(statsDoc, statsBuffer);
        sendUdp(statsBuffer);
    }

    // Process valid decoded RF packets caught by the ISR
    if (packetReady)
    {
        sendDebug(33);

        noInterrupts();
        uint64_t capturedFrame = rxBuffer;
        uint8_t capturedCount = bitCount;

        rxBuffer = 0;
        bitCount = 0;
        packetReady = false;
        interrupts();

        sendDebug(33);

        JsonDocument doc;

        // Padded to 8 hex chars now for 32-bit visualization
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "0x%08llX", capturedFrame);
        doc["raw_hex"] = hexStr;
        doc["bits_captured"] = capturedCount;

        // Shift slicing down for 4 bytes (Bits 31 down to 0)
        uint8_t b0 = (capturedFrame >> 24) & 0xFF;
        uint8_t b1 = (capturedFrame >> 16) & 0xFF;
        uint8_t b2 = (capturedFrame >> 8) & 0xFF;
        uint8_t b3 = capturedFrame & 0xFF;

        doc["b0"] = b0;
        doc["b1"] = b1;
        doc["b2"] = b2;
        doc["b3"] = b3;

        // --- Standard 32-Bit Protocol Mapping ---
        char idStr[8];
        snprintf(idStr, sizeof(idStr), "0x%02X", b0);
        doc["sensor_id"] = idStr;

        // Temperature Extraction (Typically 12 bits across b1 and b2)
        uint16_t raw_temp = ((b1 & 0x0F) << 8) | b2;
        if (raw_temp & 0x800)
        {
            doc["temperature_c"] = -((raw_temp & 0x7FF) / 10.0);
        }
        else
        {
            doc["temperature_c"] = raw_temp / 10.0;
        }

        // Humidity Extraction (Typically maps directly onto byte 3)
        doc["humidity"] = b3;

        // Battery State (Typically flag structures in b1)
        doc["battery"] = (b1 & 0x10) ? "Low" : "OK";

        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);
        sendUdp(jsonBuffer);
    }
    // Yield execution to protect core ESP8266 Wi-Fi context layers
    yield();
}