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
#define HALF_BIT_TIMEOUT 500
#define FULL_BIT_TIMEOUT 1000
#define TIMING_TOLERANCE 180

// Volatile state storage for the Manchester ISR engine
volatile uint64_t rxBuffer = 0;
volatile uint8_t bitCount = 0;
volatile bool packetReady = false;

// --- INSTRUMENTATION METRICS ---
volatile uint32_t metric_total_edges = 0;
volatile uint32_t metric_glitches    = 0;
volatile uint32_t metric_bad_timings = 0;
volatile uint32_t metric_shorts      = 0;
volatile uint32_t metric_longs       = 0;
volatile uint8_t  metric_max_bits    = 0; // Tracks the deepest bit depth reached before a reset

// Minimal ISR: Focuses strictly on edge-timing and bit-shifting
IRAM_ATTR void rf_edge_isr()
{
    metric_total_edges++;
    static unsigned long lastTime = 0;
    static bool phase = false;

    unsigned long now = micros();
    unsigned long duration = now - lastTime;
    lastTime = now;

    // Glitch filter for high-frequency ambient RF spikes
    if (duration < 150) {
        metric_glitches++;
        return;
    }

    bool state = digitalRead(RF_DATA_PIN);

    // Timing evaluation windows around 500us and 1000us benchmarks
    bool isShort = (duration > (HALF_BIT_TIMEOUT - TIMING_TOLERANCE) && duration < (HALF_BIT_TIMEOUT + TIMING_TOLERANCE));
    bool isLong = (duration > (FULL_BIT_TIMEOUT - TIMING_TOLERANCE) && duration < (FULL_BIT_TIMEOUT + TIMING_TOLERANCE));

    if (!isShort && !isLong)
    {
        // Log highest bit reached before noise killed the sequence
        if (bitCount > metric_max_bits) {
            metric_max_bits = bitCount;
        }
        // Drop incomplete chains on sequence violations
        bitCount = 0;
        phase = false;
        metric_bad_timings++;
        return;
    }

    bool sampledBit = state;

    if (isLong)
    {
        metric_longs++;
        phase = true; // Long pulse forces mid-bit tracking window alignment
    }
    else
    {
        metric_shorts++;
        if (!phase)
        {
            phase = true;
            return; // Drop boundary edge transition marks
        }
        phase = false; // Valid mid-bit cell center achieved; proceed to sample
    }

    if (!packetReady)
    {
        rxBuffer = (rxBuffer << 1) | (sampledBit ? 1 : 0);
        bitCount++;

        // Auriol V2 payloads stabilize frames across 40 distinct bits
        if (bitCount >= 40)
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
    attachInterrupt(digitalPinToInterrupt(RF_DATA_PIN), rf_edge_isr, CHANGE);

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
        uint32_t glitches   = metric_glitches;
        uint32_t badTimings = metric_bad_timings;
        uint32_t shorts     = metric_shorts;
        uint32_t longs      = metric_longs;
        uint8_t  maxBits    = metric_max_bits;
        
        // Reset diagnostic metrics for the next tracking window
        metric_total_edges = 0;
        metric_glitches    = 0;
        metric_bad_timings = 0;
        metric_shorts      = 0;
        metric_longs       = 0;
        metric_max_bits    = 0;
        interrupts();

        // Package up diagnostics payload
        JsonDocument statsDoc;
        statsDoc["type"]         = "rfrx_metrics";
        statsDoc["total_edges"]  = totalEdges;
        statsDoc["glitches"]     = glitches;
        statsDoc["bad_timings"]  = badTimings;
        statsDoc["valid_shorts"] = shorts;
        statsDoc["valid_longs"]  = longs;
        statsDoc["max_bit_depth"]= maxBits;

        char statsBuffer[256];
        serializeJson(statsDoc, statsBuffer);
        sendUdp(statsBuffer);
    }

    // Process valid decoded RF packets caught by the ISR
    if (packetReady)
    {
        // Detach interrupt memory access safely to extract volatile bits
        noInterrupts();
        uint64_t capturedFrame = rxBuffer;
        uint8_t capturedCount = bitCount;

        // Clear variables inside atomic window to re-enable background capture
        rxBuffer = 0;
        bitCount = 0;
        packetReady = false;
        interrupts();

        sendDebug(33);

        // Build the structured telemetry payload
        JsonDocument doc;

        // Output complete 40-bit sequence in a padded hex format for profile matching
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "0x%010llX", capturedFrame);
        doc["raw_hex"] = hexStr;
        doc["bits_captured"] = capturedCount;

        // Split the frame into separate diagnostic bytes (Bits 39 down to 0)
        uint8_t b0 = (capturedFrame >> 32) & 0xFF;
        uint8_t b1 = (capturedFrame >> 24) & 0xFF;
        uint8_t b2 = (capturedFrame >> 16) & 0xFF;
        uint8_t b3 = (capturedFrame >> 8) & 0xFF;
        uint8_t b4 = capturedFrame & 0xFF;

        // Map individual bytes to payload to easily identify bits via the UDP port
        doc["b0"] = b0;
        doc["b1"] = b1;
        doc["b2"] = b2;
        doc["b3"] = b3;
        doc["b4"] = b4;

        // --- Standard FineOffset / Auriol V2 Bitmask Baselines ---
        char idStr[8];
        snprintf(idStr, sizeof(idStr), "0x%02X", b0);
        doc["sensor_id"] = idStr;

        uint16_t raw_temp = ((b1 & 0x0F) << 8) | b2;
        if (raw_temp & 0x800)
        {
            doc["temperature_c"] = -((raw_temp & 0x7FF) / 10.0);
        }
        else
        {
            doc["temperature_c"] = raw_temp / 10.0;
        }

        doc["humidity"] = b3;
        doc["battery"] = (b1 & 0x10) ? "Low" : "OK";

        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);
        sendUdp(jsonBuffer);
    }

    // Yield execution to protect core ESP8266 Wi-Fi context layers
    yield();
}