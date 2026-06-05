#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WeatherStationDataRx.h>

// Pin Configuration
#define RX_POWER_PIN 0  // GPIO0 enables the receiver transistor
#define RF_DATA_PIN  3  // RX (GPIO3) for the data stream

// Network Settings
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqtt_broker = "192.168.1.50"; // Your MQTT broker IP
const int   mqtt_port   = 1883;
const char* mqtt_topic  = "tele/auriol_weather/SENSOR";

// Instantiations
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WeatherStationDataRx rxEngine;

unsigned long last_reconnect_attempt = 0;

void setup_wifi() {
    delay(10);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
}

bool reconnect_mqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    
    if (mqttClient.connect("ESP01S_433_Bridge")) {
        // Optional: Subscriptions can go here if needed
        return true;
    }
    return false;
}

void setup() {
    // 1. Immediately drive GPIO0 high to power up the receiver stage
    pinMode(RX_POWER_PIN, OUTPUT);
    digitalWrite(RX_POWER_PIN, HIGH);
    
    // 2. Configure RF Data pin and attach interrupts
    pinMode(RF_DATA_PIN, INPUT);
    rxEngine.begin(RF_DATA_PIN);
    
    // 3. Network Setup
    setup_wifi();
    mqttClient.setServer(mqtt_broker, mqtt_port);
    
    // Allow RF hardware oscillator to settle post-boot
    delay(100); 
}

void loop() {
    // Run the decoder internal state engine on every loop pass
    rxEngine.loop();

    // Handle non-blocking network connection maintenance
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - last_reconnect_attempt > 5000) { // Retry every 5 seconds
            last_reconnect_attempt = now;
            if (reconnect_mqtt()) {
                last_reconnect_attempt = 0;
            }
        }
    } else {
        mqttClient.loop();
    }

    // Process valid decoded RF packets
    if (rxEngine.available()) {
        auto sensorData = rxEngine.getCapturedData();
        
        // Structure the JSON document (ArduinoJson V7 syntax)
        JsonDocument doc;
        
        // Convert internal hex ID to string representation for clean MQTT keys
        char idStr[8];
        snprintf(idStr, sizeof(idStr), "0x%04X", sensorData.sensor_id);
        
        doc["sensor_id"]     = idStr;
        doc["channel"]       = sensorData.channel;
        doc["temperature_c"] = sensorData.temperature / 10.0; // Library uses integer fixed-point
        doc["humidity"]      = sensorData.humidity;
        doc["battery_low"]   = (sensorData.battery == 0); // Check library mapping for your version

        // Serialise to string buffer
        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);

        // Publish to MQTT broker if connected
        if (mqttClient.connected()) {
            mqttClient.publish(mqtt_topic, jsonBuffer);
        }
        
        // Yield execution back to underlying ESP8266 system tasks
        yield(); 
    }
}