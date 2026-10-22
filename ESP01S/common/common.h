#ifndef __common_h__
#define __common_h__

////////////////////////////////////////////////////////////
// Security
////////////////////////////////////////////////////////////
/* Secrets looks like this:
const char* WIFI_SSID = "blah blah";
const char* WIFI_PASS = "deadbeef";
*/
#include "secrets.h"

////////////////////////////////////////////////////////////
// Debugging Assistance: Serial
////////////////////////////////////////////////////////////
// Debug macros - comment out SERIAL_DEBUG to disable all debug output
#ifdef SERIAL_DEBUG
  #define DEBUG_PRINT(x)     Serial.print(x)
  #define DEBUG_PRINTLN(x)   Serial.println(x)
  #define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

////////////////////////////////////////////////////////////
// Debugging Assistance: UDP
////////////////////////////////////////////////////////////
// #define UDP_HEARTBEAT_PORT 4210
#ifdef UDP_HEARTBEAT_PORT
WiFiUDP udp;

/**
 * Sends a simple UDP broadcast heartbeat.
 * Called when the frame is rebuilt.
 */
void sendDebug(uint8_t value) {
    char buffer[84];
    snprintf(buffer, sizeof(buffer), "{\"status\":\"debug\",\"id\":%d,\"value\":\"%02d\"}",DEB_VER,value);
    
    udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_HEARTBEAT_PORT);
    udp.write(buffer);
    udp.endPacket();
}

void sendUdp(char* buffer) {
    udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_HEARTBEAT_PORT);
    udp.write(buffer);
    udp.endPacket();
}
#endif 
////////////////////////////////////////////////////////////
// Debugging Assistance: WiFi and OTA control
////////////////////////////////////////////////////////////

volatile bool wifi_enabled = false;

void wifi_enable() {

    if (wifi_enabled) {
        return;
    }

    DEBUG_PRINT("\nWiFi connecting...");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    // Hostname must be set BEFORE begin()
    WiFi.hostname(WIFI_HOSTNAME);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int retryCount = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        DEBUG_PRINT(".");
        if (++retryCount > 40) { // Approx 20 seconds
            ESP.restart();
        }
    }
    DEBUG_PRINT("\nWiFi connected!");

    // OTA must be reinitialized after WiFi reconnect
    ArduinoOTA.setHostname(WIFI_HOSTNAME);

    ArduinoOTA.onStart([]() {
        DEBUG_PRINTLN("[OTA] Update sequence starting. Disabling RF output...");
        sendDebug(0);
        disableRF();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      DEBUG_PRINTF("[OTA] Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onEnd([]() {
        sendDebug(1);
        delay(100);
        DEBUG_PRINTLN("\n[OTA] Complete. Rebooting...");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        sendDebug(2);
        DEBUG_PRINTF("[OTA] Error[%u]\n", error);
    });

    ArduinoOTA.begin();

    wifi_enabled = true;

    sendDebug(DEB_VER);
}

////////////////////////////////////////////////////////////
// Runtime WiFi health check
////////////////////////////////////////////////////////////
// wifi_enable() only runs once at boot. If the link drops later (AP reboot,
// DHCP lease expiry, interference) nothing re-establishes it: wifi_enabled
// stays true, MQTT can never reconnect, and the device goes silent without
// rebooting. Call wifi_check() every loop pass: it nudges the SDK to
// reconnect and, if the link stays down past WIFI_RECOVERY_TIMEOUT, reboots
// so the device recovers on its own (matching the boot-time connect-or-restart
// policy in wifi_enable()).
#ifndef WIFI_RECOVERY_TIMEOUT
#define WIFI_RECOVERY_TIMEOUT (60 * 1000UL) // Reboot after 60s of no link
#endif
#ifndef WIFI_RECONNECT_INTERVAL
#define WIFI_RECONNECT_INTERVAL (5 * 1000UL) // Nudge reconnect every 5s
#endif

void wifi_check() {
    if (!wifi_enabled) {
        return;
    }

    static unsigned long wifi_down_since = 0; // 0 = link currently healthy
    static unsigned long last_reconnect = 0;

    if (WiFi.status() == WL_CONNECTED) {
        wifi_down_since = 0; // Clear the outage timer.
        return;
    }

    unsigned long now = millis();

    // First pass we notice the outage: start the timer and force an immediate
    // reconnect attempt below.
    if (wifi_down_since == 0) {
        wifi_down_since = now;
        last_reconnect = 0;
        DEBUG_PRINTLN("\nWiFi link lost; attempting recovery...");
    }

    // Give up and reboot if the link has been down too long. A clean restart
    // fully reinitialises WiFi, OTA and MQTT.
    if (now - wifi_down_since > WIFI_RECOVERY_TIMEOUT) {
        sendDebug(12);
        ESP.restart();
    }

    // Periodically nudge the SDK to re-establish the connection.
    if (last_reconnect == 0 || now - last_reconnect > WIFI_RECONNECT_INTERVAL) {
        last_reconnect = now;
        WiFi.reconnect();
    }
}

void wifi_disable() {

    if (!wifi_enabled) {
        return;
    }

    sendDebug(11);
    delay(100);

    // Disconnect cleanly
    WiFi.disconnect(true);

    delay(10);

    // Disable STA
    WiFi.mode(WIFI_OFF);

    // Force RF subsystem sleep
    WiFi.forceSleepBegin();

    delay(1);

    wifi_enabled = false;

    DEBUG_PRINTLN("\nWiFi disconnected.");
}

////////////////////////////////////////////////////////////
// HARDWARE ABSTRACTION
////////////////////////////////////////////////////////////
// LED control
#ifdef LED_PIN
volatile bool led_enabled = false;
inline void ledEnable() { pinMode(LED_PIN, OUTPUT); led_enabled = true;}
inline void ledOn()     { if(led_enabled) digitalWrite(LED_PIN, HIGH); }
inline void ledOff()    { if(led_enabled) digitalWrite(LED_PIN, LOW); }
inline void ledDisable(){ pinMode(LED_PIN, INPUT); led_enabled = false;}
#endif

#endif