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
// Pulse timing in microseconds
#define TYPE1_SHORT_US 2400
#define TYPE1_LONG_US 4400
#define TYPE2_SHORT_US 1400
#define TYPE2_LONG_US 2400

#define GLITCH_MIN_US 600
#define TRAIN_RESET_US 15000
#define TRAIN_RESET_HOLD_COUNT 3
#define PROFILE_LOCK_MIN_SAMPLES 12
#define PROFILE_LOCK_FORCE_SAMPLES 24
#define PACKET_MIN_BITS 26
#define PACKET_MAX_BITS 52

// ISR queue: timing deltas only, decode in loop() to keep ISR tiny.
#define ISR_QUEUE_SIZE 128
#define ISR_QUEUE_MASK (ISR_QUEUE_SIZE - 1)

volatile uint16_t isrDurations[ISR_QUEUE_SIZE];
volatile uint8_t isrHead = 0;
volatile uint8_t isrTail = 0;

// ISR instrumentation
volatile uint32_t metric_total_edges = 0;
volatile uint32_t metric_glitch_edges = 0;
volatile uint32_t metric_queue_overflow = 0;

// Loop-side instrumentation
uint32_t metric_processed_intervals = 0;
uint32_t metric_bad_intervals = 0;
uint32_t metric_train_resets = 0;
uint32_t metric_profile_locks_type1 = 0;
uint32_t metric_profile_locks_type2 = 0;
uint32_t metric_gap_detected = 0;
uint32_t metric_packet_candidates = 0;
uint32_t metric_packet_invalid_len = 0;
uint32_t metric_repeat_hits = 0;
uint32_t metric_consensus_hits = 0;
uint32_t metric_type1_decode_ok = 0;
uint32_t metric_type1_decode_unknown = 0;
uint32_t metric_type2_decode_ok = 0;
uint32_t metric_type2_decode_unknown = 0;
uint8_t metric_max_bits = 0;

enum DecoderProfile : uint8_t
{
    PROFILE_UNKNOWN = 0,
    PROFILE_TYPE1 = 1,
    PROFILE_TYPE2 = 2,
};

DecoderProfile activeProfile = PROFILE_UNKNOWN;
bool waitForPacketStart = true;

uint64_t workBits = 0;
uint8_t workBitCount = 0;

uint64_t lastCandidateBits = 0;
uint8_t lastCandidateBitsCount = 0;
DecoderProfile lastCandidateProfile = PROFILE_UNKNOWN;
uint8_t lastCandidateRepeatCount = 0;
bool lastCandidateConsensusSent = false;

uint32_t lockScoreType1 = 0;
uint32_t lockScoreType2 = 0;
uint8_t lockSamples = 0;
uint8_t trainResetStreak = 0;

static inline uint16_t absDiffU16(uint16_t a, uint16_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static inline uint16_t timingTolUs(uint16_t nominal)
{
    uint16_t pct = nominal / 5; // 20%
    return (pct < 250) ? 250 : pct;
}

static inline bool inWindow(uint16_t value, uint16_t nominal)
{
    uint16_t tol = timingTolUs(nominal);
    return (value >= (nominal - tol) && value <= (nominal + tol));
}

static inline uint8_t classifySymbol(uint16_t dt, DecoderProfile profile)
{
    if (profile == PROFILE_TYPE1)
    {
        if (inWindow(dt, TYPE1_SHORT_US))
            return 0;
        if (inWindow(dt, TYPE1_LONG_US))
            return 1;
        return 0xFF;
    }
    if (profile == PROFILE_TYPE2)
    {
        if (inWindow(dt, TYPE2_SHORT_US))
            return 0;
        if (inWindow(dt, TYPE2_LONG_US))
            return 1;
        return 0xFF;
    }
    return 0xFF;
}

static inline uint16_t profileLongUs(DecoderProfile profile)
{
    return (profile == PROFILE_TYPE2) ? TYPE2_LONG_US : TYPE1_LONG_US;
}

static inline void resetPacketAssembler()
{
    workBits = 0;
    workBitCount = 0;
}

static inline void resetCandidateHistory()
{
    lastCandidateBits = 0;
    lastCandidateBitsCount = 0;
    lastCandidateProfile = PROFILE_UNKNOWN;
    lastCandidateRepeatCount = 0;
    lastCandidateConsensusSent = false;
}

static inline void resetProfileLock()
{
    activeProfile = PROFILE_UNKNOWN;
    lockScoreType1 = 0;
    lockScoreType2 = 0;
    lockSamples = 0;
    trainResetStreak = 0;
    waitForPacketStart = true;
    resetPacketAssembler();
    resetCandidateHistory();
}

static inline bool popIsrDuration(uint16_t *outDt)
{
    bool hasItem = false;
    noInterrupts();
    if (isrTail != isrHead)
    {
        *outDt = isrDurations[isrTail];
        isrTail = (isrTail + 1) & ISR_QUEUE_MASK;
        hasItem = true;
    }
    interrupts();
    return hasItem;
}

void emitDecoderEvent(const char *eventType, uint16_t dt, bool includePacket)
{
    JsonDocument doc;
    doc["type"] = eventType;
    doc["dt_us"] = dt;
    doc["profile"] = (activeProfile == PROFILE_TYPE1) ? "type1" : ((activeProfile == PROFILE_TYPE2) ? "type2" : "unknown");
    doc["lock_samples"] = lockSamples;

    if (includePacket)
    {
        doc["bits_captured"] = workBitCount;
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "0x%016llX", workBits);
        doc["raw_hex"] = hexStr;
        doc["repeat_count"] = lastCandidateRepeatCount;
    }

    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);
    sendUdp(jsonBuffer);
}

static inline void emitType1DecodeEvent(uint64_t rawBits, uint8_t bitCount, uint16_t boundaryDt)
{
    JsonDocument doc;
    doc["type"] = "type1_decode";
    doc["profile"] = "type1";
    doc["dt_us"] = boundaryDt;
    doc["bits_captured"] = bitCount;

    char hexStr[32];
    snprintf(hexStr, sizeof(hexStr), "0x%016llX", rawBits);
    doc["raw_hex"] = hexStr;

    if (bitCount != 32)
    {
        doc["decoded"] = false;
        doc["reason"] = "unexpected_bit_length";
        metric_type1_decode_unknown++;
    }
    else
    {
        uint32_t raw32 = (uint32_t)(rawBits & 0xFFFFFFFFUL);
        uint8_t b0 = (raw32 >> 24) & 0xFF;
        uint8_t b1 = (raw32 >> 16) & 0xFF;
        uint8_t b2 = (raw32 >> 8) & 0xFF;
        uint8_t b3 = raw32 & 0xFF;

        // Type 1: b1 low nibble + b2 forms signed 12-bit temp in 0.1C steps.
        int16_t rawTemp = ((int16_t)(b1 & 0x0F) << 8) | b2;
        if (rawTemp >= 2048)
            rawTemp -= 4096;

        float finalTemp = rawTemp / 10.0f;

        doc["decoded"] = true;
        doc["temperature_c"] = finalTemp;
        doc["battery"] = (b0 & 0x80) ? "Low" : "OK";

        char idStr[8];
        snprintf(idStr, sizeof(idStr), "0x%02X", b0);
        doc["sensor_id"] = idStr;
        doc["checksum"] = b3;

        if (b0 == 0x0C)
            doc["sender"] = "inside";
        else if (b0 == 0xE6)
            doc["sender"] = "outside";
        else
            doc["sender"] = "unknown";

        metric_type1_decode_ok++;
    }

    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);
    sendUdp(jsonBuffer);
}

static inline void emitType2DecodeEvent(uint64_t rawBits, uint8_t bitCount, uint16_t boundaryDt)
{
    JsonDocument doc;
    doc["type"] = "type2_decode";
    doc["profile"] = "type2";
    doc["dt_us"] = boundaryDt;
    doc["bits_captured"] = bitCount;

    char hexStr[32];
    snprintf(hexStr, sizeof(hexStr), "0x%016llX", rawBits);
    doc["raw_hex"] = hexStr;

    if (bitCount != 36)
    {
        doc["decoded"] = false;
        doc["reason"] = "unexpected_bit_length";
        metric_type2_decode_unknown++;
    }
    else
    {
        // Treat the 36-bit payload as 9 nibbles: n0..n8 (MSB to LSB).
        uint64_t raw36 = rawBits & 0xFFFFFFFFFULL;
        uint8_t n0 = (raw36 >> 32) & 0x0F;
        uint8_t n1 = (raw36 >> 28) & 0x0F;
        uint8_t n2 = (raw36 >> 24) & 0x0F;
        uint8_t n3 = (raw36 >> 20) & 0x0F;
        uint8_t n4 = (raw36 >> 16) & 0x0F;
        uint8_t n5 = (raw36 >> 12) & 0x0F;
        uint8_t n7 = (raw36 >> 4) & 0x0F;
        uint8_t n8 = raw36 & 0x0F;

        uint16_t rawTemp = ((uint16_t)n3 << 8) | ((uint16_t)n4 << 4) | n5;
        float finalTemp = rawTemp / 10.0f;

        // Updated mapping from validated captures:
        // humidity maps directly to the low byte, channel maps from n2 LSB.
        uint8_t humidityWire = (uint8_t)((n7 << 4) | n8);
        uint8_t humidity = humidityWire;
        uint8_t channel = (n2 & 0x03) + 1;

        doc["decoded"] = true;
        doc["temperature_c"] = finalTemp;
        doc["humidity"] = humidity;
        doc["channel"] = channel;
        doc["humidity_wire"] = humidityWire;
        doc["decode_model"] = "provisional";

        char idStr[8];
        snprintf(idStr, sizeof(idStr), "0x%01X%01X", n0, n1);
        doc["sensor_id"] = idStr;

        metric_type2_decode_ok++;
    }

    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);
    sendUdp(jsonBuffer);
}

static inline void finalizeCandidate(uint16_t boundaryDt, bool allowEmit)
{
    if (workBitCount == 0)
        return;

    if (workBitCount > metric_max_bits)
        metric_max_bits = workBitCount;

    if (workBitCount < PACKET_MIN_BITS || workBitCount > PACKET_MAX_BITS)
    {
        metric_packet_invalid_len++;
        resetPacketAssembler();
        return;
    }

    metric_packet_candidates++;

    if (lastCandidateProfile == activeProfile &&
        lastCandidateBitsCount == workBitCount &&
        lastCandidateBits == workBits)
    {
        if (lastCandidateRepeatCount < 255)
            lastCandidateRepeatCount++;
        metric_repeat_hits++;
    }
    else
    {
        lastCandidateProfile = activeProfile;
        lastCandidateBitsCount = workBitCount;
        lastCandidateBits = workBits;
        lastCandidateRepeatCount = 1;
        lastCandidateConsensusSent = false;
    }

    if (allowEmit && lastCandidateRepeatCount == 1)
    {
        emitDecoderEvent("decoder_candidate", boundaryDt, true);
    }

    if (allowEmit && lastCandidateRepeatCount >= 2 && !lastCandidateConsensusSent)
    {
        metric_consensus_hits++;
        emitDecoderEvent("decoder_consensus", boundaryDt, true);

        // Process payloads only on first consensus event.
        if (activeProfile == PROFILE_TYPE1)
            emitType1DecodeEvent(workBits, workBitCount, boundaryDt);
            else if (activeProfile == PROFILE_TYPE2)
                emitType2DecodeEvent(workBits, workBitCount, boundaryDt);

        lastCandidateConsensusSent = true;
    }

    resetPacketAssembler();
}

IRAM_ATTR void rf_edge_isr()
{
    static unsigned long lastRisingTime = 0;

    unsigned long now = micros();
    if (lastRisingTime == 0)
    {
        lastRisingTime = now;
        return;
    }

    unsigned long rawDuration = now - lastRisingTime;
    lastRisingTime = now;
    metric_total_edges++;

    if (rawDuration < GLITCH_MIN_US)
    {
        metric_glitch_edges++;
        return;
    }

    uint16_t duration = (rawDuration > 65535UL) ? 65535 : (uint16_t)rawDuration;
    uint8_t nextHead = (isrHead + 1) & ISR_QUEUE_MASK;
    if (nextHead == isrTail)
    {
        metric_queue_overflow++;
        return;
    }

    isrDurations[isrHead] = duration;
    isrHead = nextHead;
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

    // Consume queued timing intervals from ISR and run instrumentation decoder state machine.
    uint16_t dt = 0;
    while (popIsrDuration(&dt))
    {
        metric_processed_intervals++;

        if (dt >= TRAIN_RESET_US)
        {
            if (activeProfile != PROFILE_UNKNOWN || workBitCount > 0)
                metric_train_resets++;

            // Train reset boundaries are state transitions, not packet events.
            finalizeCandidate(dt, false);
            resetPacketAssembler();
            waitForPacketStart = true;
            resetCandidateHistory();

            if (trainResetStreak < 255)
                trainResetStreak++;

            // Hold lock for a few long silences to reduce profile flapping.
            if (trainResetStreak >= TRAIN_RESET_HOLD_COUNT)
                resetProfileLock();
            continue;
        }

        trainResetStreak = 0;

        if (activeProfile == PROFILE_UNKNOWN)
        {
            uint16_t d1 = absDiffU16(dt, TYPE1_SHORT_US);
            uint16_t d2 = absDiffU16(dt, TYPE1_LONG_US);
            lockScoreType1 += (d1 < d2) ? d1 : d2;

            uint16_t d3 = absDiffU16(dt, TYPE2_SHORT_US);
            uint16_t d4 = absDiffU16(dt, TYPE2_LONG_US);
            lockScoreType2 += (d3 < d4) ? d3 : d4;

            if (lockSamples < 255)
                lockSamples++;

            bool canLock = false;
            if (lockSamples >= PROFILE_LOCK_MIN_SAMPLES)
            {
                uint32_t scoreDiff = (lockScoreType1 > lockScoreType2) ? (lockScoreType1 - lockScoreType2) : (lockScoreType2 - lockScoreType1);
                if (scoreDiff > ((uint32_t)lockSamples * 120))
                    canLock = true;
            }

            if (!canLock && lockSamples >= PROFILE_LOCK_FORCE_SAMPLES)
                canLock = true;

            if (canLock)
            {
                activeProfile = (lockScoreType1 <= lockScoreType2) ? PROFILE_TYPE1 : PROFILE_TYPE2;
                waitForPacketStart = true;
                resetCandidateHistory();
                if (activeProfile == PROFILE_TYPE1)
                    metric_profile_locks_type1++;
                else
                    metric_profile_locks_type2++;

                emitDecoderEvent("decoder_profile_lock", dt, false);
            }
            continue;
        }

        uint8_t bitVal = classifySymbol(dt, activeProfile);
        if (bitVal != 0xFF)
        {
            if (!waitForPacketStart)
            {
                workBits = (workBits << 1) | bitVal;
                if (workBitCount < 255)
                    workBitCount++;

                if (workBitCount >= 64)
                {
                    metric_bad_intervals++;
                    resetPacketAssembler();
                    waitForPacketStart = true;
                }
            }
            continue;
        }

        uint16_t gapFloor = (uint16_t)(profileLongUs(activeProfile) + (profileLongUs(activeProfile) / 2));
        if (dt >= gapFloor)
        {
            metric_gap_detected++;
            if (waitForPacketStart)
            {
                // First gap after lock marks a packet boundary; start collecting from next symbol.
                waitForPacketStart = false;
                resetPacketAssembler();
            }
            else
            {
                    finalizeCandidate(dt, true);
            }
            continue;
        }

        metric_bad_intervals++;
    }

    // Periodic 5-second instrumentation diagnostics report
    if (currentMillis - last_stats_report >= 5000)
    {
        // Prepare for payload
        sendDebug(34);

        last_stats_report = currentMillis;

        // Perform an atomic read-and-clear of volatile metric registers
        noInterrupts();
        uint32_t totalEdges = metric_total_edges;
        uint32_t glitchEdges = metric_glitch_edges;
        uint32_t queueOverflow = metric_queue_overflow;
        uint32_t processedIntervals = metric_processed_intervals;
        uint32_t badIntervals = metric_bad_intervals;
        uint32_t trainResets = metric_train_resets;
        uint32_t lockType1 = metric_profile_locks_type1;
        uint32_t lockType2 = metric_profile_locks_type2;
        uint32_t gaps = metric_gap_detected;
        uint32_t packetCandidates = metric_packet_candidates;
        uint32_t packetInvalidLen = metric_packet_invalid_len;
        uint32_t repeatHits = metric_repeat_hits;
        uint32_t consensusHits = metric_consensus_hits;
        uint32_t type1DecodeOk = metric_type1_decode_ok;
        uint32_t type1DecodeUnknown = metric_type1_decode_unknown;
        uint32_t type2DecodeOk = metric_type2_decode_ok;
        uint32_t type2DecodeUnknown = metric_type2_decode_unknown;
        uint8_t maxBits = metric_max_bits;

        // Reset diagnostic metrics for the next tracking window
        metric_total_edges = 0;
        metric_glitch_edges = 0;
        metric_queue_overflow = 0;
        metric_processed_intervals = 0;
        metric_bad_intervals = 0;
        metric_train_resets = 0;
        metric_profile_locks_type1 = 0;
        metric_profile_locks_type2 = 0;
        metric_gap_detected = 0;
        metric_packet_candidates = 0;
        metric_packet_invalid_len = 0;
        metric_repeat_hits = 0;
        metric_consensus_hits = 0;
        metric_type1_decode_ok = 0;
        metric_type1_decode_unknown = 0;
        metric_type2_decode_ok = 0;
        metric_type2_decode_unknown = 0;
        metric_max_bits = 0;
        interrupts();

        // Package up diagnostics payload
        JsonDocument statsDoc;
        statsDoc["type"] = "decoder_metrics";
        statsDoc["total_edges"] = totalEdges;
        statsDoc["glitch_edges"] = glitchEdges;
        statsDoc["queue_overflow"] = queueOverflow;
        statsDoc["processed_intervals"] = processedIntervals;
        statsDoc["bad_intervals"] = badIntervals;
        statsDoc["train_resets"] = trainResets;
        statsDoc["locks_type1"] = lockType1;
        statsDoc["locks_type2"] = lockType2;
        statsDoc["gap_detected"] = gaps;
        statsDoc["packet_candidates"] = packetCandidates;
        statsDoc["packet_invalid_len"] = packetInvalidLen;
        statsDoc["repeat_hits"] = repeatHits;
        statsDoc["consensus_hits"] = consensusHits;
        statsDoc["type1_decode_ok"] = type1DecodeOk;
        statsDoc["type1_decode_unknown"] = type1DecodeUnknown;
        statsDoc["type2_decode_ok"] = type2DecodeOk;
        statsDoc["type2_decode_unknown"] = type2DecodeUnknown;
        statsDoc["max_bit_depth"] = maxBits;
        statsDoc["active_profile"] = (activeProfile == PROFILE_TYPE1) ? "type1" : ((activeProfile == PROFILE_TYPE2) ? "type2" : "unknown");
        statsDoc["wait_for_packet_start"] = waitForPacketStart;

        char statsBuffer[2048];
        serializeJson(statsDoc, statsBuffer);
        sendUdp(statsBuffer);
    }

    // Yield execution to protect core ESP8266 Wi-Fi context layers
    yield();
}