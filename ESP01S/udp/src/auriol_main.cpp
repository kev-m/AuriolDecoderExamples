/**
 * Project: ESP-01S 433MHz Auriol sensor decoder (Type 1: Temperature)
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

// Increase buffer to handle up to 64 bits seamlessly
IRAM_ATTR void rf_edge_isr()
{
    metric_total_edges++;
    static unsigned long lastRisingTime = 0;

    unsigned long now = micros();
    unsigned long duration = now - lastRisingTime;
    lastRisingTime = now;

    // Glitch protector: ignore edges that double-trigger too fast
    if (duration < 1000)
        return; // Ignore fast double-triggers

    // Evaluate the total cycle length from the previous rising edge to this one
    bool isShort = (duration > (CYCLE_SHORT_EXPECTED - PDM_TOLERANCE) && duration < (CYCLE_SHORT_EXPECTED + PDM_TOLERANCE));
    bool isLong = (duration > (CYCLE_LONG_EXPECTED - PDM_TOLERANCE) && duration < (CYCLE_LONG_EXPECTED + PDM_TOLERANCE));

    // A packet preamble or sync gap typically presents as a huge duration (>8000us)
    if (!isShort && !isLong)
    {
        // Always track the deepest frame seen, whether it succeeds or not
        if (bitCount > metric_max_bits)
            metric_max_bits = bitCount;

        // Trailing Sync Gap: If we've collected a plausible data frame size,
        // flag it as complete for the main loop to evaluate.
        if (bitCount >= 28 && bitCount <= 44)
        {
            packetReady = true;
        }
        else
        {
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
        uint8_t sampledBit = isLong ? 1 : 0;

        // Pack LSB-First: Always shift incoming bits into the highest slot
        rxBuffer = (rxBuffer >> 1) | ((uint64_t)sampledBit << 63);
        bitCount++;

        // Safety hard stop if noise runs wild
        if (bitCount >= 60)
        {
            bitCount = 0;
            rxBuffer = 0;
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
        noInterrupts();
        uint64_t capturedFrame = rxBuffer;
        uint8_t capturedCount = bitCount;

        rxBuffer = 0;
        bitCount = 0;
        packetReady = false;
        interrupts();

        // Right-align the payload bits inside a working 64-bit space
        // because LSB packing places shorter streams on the left side (MSB end)
        capturedFrame = capturedFrame >> (64 - capturedCount);

        JsonDocument doc;
        doc["bits_captured"] = capturedCount;

        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "0x%011llX", capturedFrame);
        doc["raw_hex"] = hexStr;

        if (capturedCount >= 28 && capturedCount <= 34)
        {
            // Lambda function to reverse bit order within a single byte
            auto reverseByte = [](uint8_t b) -> uint8_t
            {
                b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
                b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
                b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
                return b;
            };

            // Reverse every byte individually to correct the hardware reflection
            uint8_t r0 = reverseByte((capturedFrame >> 24) & 0xFF);
            uint8_t r1 = reverseByte((capturedFrame >> 16) & 0xFF);
            uint8_t r2 = reverseByte((capturedFrame >> 8) & 0xFF);
            uint8_t r3 = reverseByte(capturedFrame & 0xFF);

            doc["r0"] = r0;
            doc["r1"] = r1;
            doc["r2"] = r2;
            doc["r3"] = r3;

            // r3 = first transmitted byte = sensor rolling ID
            // r0 = last transmitted byte = checksum (varies per burst)
            char idStr[8];
            snprintf(idStr, sizeof(idStr), "0x%02X", r3);
            doc["sensor_id"] = idStr;
            doc["type"] = "Temp Only";

            // Auriol protocol: byte1=r2 carries batt+channel+temp_MSB_nibble,
            // byte2=r1 carries temp LSB byte. Together they form a 12-bit signed
            // temperature value in units of 0.1°C (two's complement).
            int16_t raw_temp = ((int16_t)(r2 & 0x0F) << 8) | r1;
            if (raw_temp >= 2048) raw_temp -= 4096; // sign-extend 12-bit to 16-bit

            float final_temp = raw_temp / 10.0;

            doc["temperature_c"] = final_temp;
            // r2 bit 7 = battery low flag; bit 6 = TX sequence counter (toggles each burst)
            doc["battery"] = (r2 & 0x80) ? "Low" : "OK";
        }
        else if (capturedCount >= 38 && capturedCount <= 44)
        {
            // --- PROFILE B: Temp & Humidity Combined (Typically 40 Bits) ---
            uint8_t b0 = (capturedFrame >> 32) & 0xFF;
            uint8_t b1 = (capturedFrame >> 24) & 0xFF; // Stable ID
            uint8_t b2 = (capturedFrame >> 16) & 0xFF;
            uint8_t b3 = (capturedFrame >> 8) & 0xFF;
            uint8_t b4 = capturedFrame & 0xFF;

            doc["b0"] = b0;
            doc["b1"] = b1;
            doc["b2"] = b2;
            doc["b3"] = b3;
            doc["b4"] = b4;

            char idStr[8];
            snprintf(idStr, sizeof(idStr), "0x%02X", b1);
            doc["sensor_id"] = idStr;
            doc["type"] = "Temp + Hum";

            // Combined extraction block for longer variant mapping
            uint16_t raw_temp = (b2 << 4) | (b3 >> 4);
            doc["temperature_c"] = (raw_temp - 400) / 10.0;

            // Extract humidity (usually maps cleanly on b3 trailing nibble or b4)
            doc["humidity"] = b4;
        }

        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);
        sendUdp(jsonBuffer);
    }
    // Yield execution to protect core ESP8266 Wi-Fi context layers
    yield();
}