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
#include <PubSubClient.h>
#include <FS.h>

#include <AuriolDecoder.h>
#include "sensor_mappings.h"
#include "web_server.h"

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

const char *mqtt_broker = "mqtt.lan"; // Your MQTT broker IP
const int mqtt_port = 1883;
const char *mqtt_topic_prefix = "home/auriol_sensor";

// Instantiations
WiFiClient espClient;
PubSubClient mqttClient(espClient);
SensorMappings sensorMappings;

unsigned long last_reconnect_attempt = 0;
unsigned long last_publish_time = 0;
#define PUBLISH_INTERVAL (60 * 1000) // 60 seconds

void enableRF()
{
    digitalWrite(RX_POWER_PIN, LOW);
}

void disableRF()
{
    digitalWrite(RX_POWER_PIN, HIGH);
}

// Clear a persisted (retained) friendly-name topic on the broker.
// Publishing an empty payload with the retain flag removes the retained message,
// so a deleted friendly name doesn't leave a stale value lingering for subscribers.
void clearRetainedTopic(const char* name)
{
    if (!mqttClient.connected() || name == nullptr || name[0] == '\0')
        return;

    char topic_name[64];
    snprintf(topic_name, sizeof(topic_name), "%s/%s", mqtt_topic_prefix, name);
    mqttClient.publish(topic_name, "", true); // empty + retain clears the topic
}

/////////////////////////////////////////////////////////////////////////////
const char* states[3] = {"Good", "Overdue", "Offline"};

#define MAX_SENSORS 10
#define OVERDUE_INTERVAL (5 * 60 * 1000)       // 5 minutes
#define OFFLINE_INTERVAL (30 * 60 * 1000)      // 30 minutes
#define REMOVE_INTERVAL (60 * 60 * 1000)       // 1 hour

typedef struct {
    uint16_t sensor_id;
    uint8_t channel;
    float temperature_c;
    uint8_t humidity_pct;
    bool battery_ok;
    // Status: 0 = good, 1 = overdue, 2 = offline
    uint8_t status;
    unsigned long last_update_time;
} sensor_t;

sensor_t sensor_array[MAX_SENSORS];
uint8_t active_sensor_count = 0;
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
// Callback invoked when sensor packet is decoded
void onSensorPacket(uint16_t sensor_id, uint8_t channel, float temperature_c, uint8_t humidity_pct, bool battery_ok)
{
    unsigned long now = millis();

    // Search for existing sensor
    for (uint8_t i = 0; i < active_sensor_count; i++)
    {
        if (sensor_array[i].sensor_id == sensor_id && sensor_array[i].channel == channel)
        {
            // Update existing sensor
            sensor_array[i].temperature_c = temperature_c;
            sensor_array[i].humidity_pct = humidity_pct;
            sensor_array[i].battery_ok = battery_ok;
            sensor_array[i].last_update_time = now;
            sensor_array[i].status = 0; // Reset to "Good"
            return;
        }
    }

    // Add new sensor if there's space
    if (active_sensor_count < MAX_SENSORS)
    {
        sensor_array[active_sensor_count].sensor_id = sensor_id;
        sensor_array[active_sensor_count].channel = channel;
        sensor_array[active_sensor_count].temperature_c = temperature_c;
        sensor_array[active_sensor_count].humidity_pct = humidity_pct;
        sensor_array[active_sensor_count].battery_ok = battery_ok;
        sensor_array[active_sensor_count].status = 0; // Good
        sensor_array[active_sensor_count].last_update_time = now;
        active_sensor_count++;
    }
}
// Useful for reverse engineering a payload (e.g. finding the real battery bit).
void onRawFrame(uint8_t profile, uint64_t raw_bits, uint8_t bit_count, uint8_t repeat_count) {
  const char *profileName = (profile == AURIOL_PROFILE_TYPE1) ? "Type1" :
                             (profile == AURIOL_PROFILE_TYPE2) ? "Type2" : "Unknown";

    //sendDebug(14);
    char jsonBuffer[1024];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
        "{"
        "\"type\":\"Auriol\","
        "\"profile\":\"%4s\","
        "\"raw_bits\":\"0x%016llX\","
        "\"bit_count\":%u,"
        "\"repeat_count\":%u"
        "}",
        profileName,
        raw_bits,
        bit_count,
        repeat_count
    );

    sendUdp(jsonBuffer);
}
/////////////////////////////////////////////////////////////////////////////
void publish_topics(void)
{
    unsigned long now = millis();

    // Clean up and update statuses
    for (int8_t i = (int8_t)active_sensor_count - 1; i >= 0; i--)
    {
        unsigned long elapsed = now - sensor_array[i].last_update_time;

        // Remove if offline for more than 1 hour
        if (elapsed > REMOVE_INTERVAL)
        {
            // Shift array
            for (uint8_t j = i; j < active_sensor_count - 1; j++)
            {
                sensor_array[j] = sensor_array[j + 1];
            }
            active_sensor_count--;
            continue;
        }

        // Update status based on elapsed time
        if (elapsed > OFFLINE_INTERVAL)
        {
            sensor_array[i].status = 2; // Offline
        }
        else if (elapsed > OVERDUE_INTERVAL)
        {
            sensor_array[i].status = 1; // Overdue
        }
    }

    // Publish active sensors to MQTT
    if (mqttClient.connected())
    {
        for (uint8_t i = 0; i < active_sensor_count; i++)
        {
            sensor_t* sensor = &sensor_array[i];

            // Apply per-sensor linear correction (identity for unmapped sensors).
            float corrected_temp = sensorMappings.applyCorrection(sensor->sensor_id, sensor->channel, sensor->temperature_c);

            char jsonBuffer[1024];
            snprintf(jsonBuffer, sizeof(jsonBuffer),
                     "{"
                     "\"type\":\"Auriol\","
                     "\"sensor_id\":\"%04X\","
                     "\"channel\":%u,"
                     "\"temperature_c\":%.1f,"
                     "\"humidity_pct\":%u,"
                     "\"battery\":\"%s\","
                     "\"state\":\"%s\""
                     "}",
                     sensor->sensor_id,
                     (unsigned int)sensor->channel,
                     corrected_temp,
                     sensor->humidity_pct,
                     sensor->battery_ok ? "OK" : "Low",
                     states[sensor->status]);

            char topic_name[64];
            bool retain = false;
            const char* mapped_name = sensorMappings.getName(sensor->sensor_id, sensor->channel);
            if (mapped_name)
            {
                snprintf(topic_name, sizeof(topic_name),
                         "%s/%s",
                         mqtt_topic_prefix,
                         mapped_name);
                // Persist friendly-name topics so subscribers get the last value on connect.
                retain = true;
            }
            else
            {
                snprintf(topic_name, sizeof(topic_name),
                         "%s/ID/%04X",
                         mqtt_topic_prefix,
                         sensor->sensor_id);
            }
            mqttClient.publish(topic_name, jsonBuffer, retain);
        }
    }
}
/////////////////////////////////////////////////////////////////////////////
bool reconnect_mqtt()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;

    if (mqttClient.connect("433_Bridge"))
    {
        // Optional: Subscriptions can go here if needed
        return true;
    }
    return false;
}

void setup()
{
    // 1. Immediately drive GPIO0 high to power up the receiver stage
    pinMode(RX_POWER_PIN, OUTPUT);
    enableRF();

    // 2. Initialise the sensor array to empty
    active_sensor_count = 0;
    memset(sensor_array, 0, sizeof(sensor_array));

    // 3. Configure RF Data pin and attach interrupts
    auriolDecoderSetup(RF_DATA_PIN, onSensorPacket);
    // auriolDecoderSetDebugCallback(onRawFrame);  

    // 4. Initialize LittleFS and sensor mappings
    LittleFS.begin();
    sensorMappings.init();

    // 5. Network Setup
    wifi_enable();
    mqttClient.setServer(mqtt_broker, mqtt_port);
    setupWebServer();

    // 6. Allow RF hardware oscillator to settle post-boot
    delay(100);
}

void loop()
{
    // Re-establish WiFi if the link dropped since boot (reboots on prolonged
    // outage). Without this the device goes silent and never recovers.
    wifi_check();

    if (wifi_enabled)
    {
        ArduinoOTA.handle();
        handleWebServer();
    }

    // Run the decoder internal state engine on every loop pass
    auriolDecoderProcess();

    // Handle non-blocking network connection maintenance
    if (!mqttClient.connected())
    {
        unsigned long now = millis();
        if (now - last_reconnect_attempt > 5000)
        { // Retry every 5 seconds
            last_reconnect_attempt = now;
            if (reconnect_mqtt())
            {
                last_reconnect_attempt = 0;
            }
        }
    }
    else
    {
        mqttClient.loop();
    }

    // Publish sensor data once per minute
    unsigned long now = millis();
    if (now - last_publish_time > PUBLISH_INTERVAL)
    {
        last_publish_time = now;
        publish_topics();
    }

    yield();
}