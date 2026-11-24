# Auriol Examples

This repository contains **3 different implementations** of the [AuriolDecoder](https://github.com/kev-m/AuriolDecoder) library, demonstrating how to use the decoder library in various IoT scenarios.

> **Note:** The decoder protocol implementation details are in the [AuriolDecoder](https://github.com/kev-m/AuriolDecoder) library repository. This repo focuses on example integrations.

## Implementations

1. **demo** ([ESP01S/demo/](ESP01S/demo/)) - Writes the decoded data to the serial port.
2. **UDP** ([ESP01S/udp/](ESP01S/udp/)) - Broadcasts decoded sensor data via UDP (port 4242) as JSON payloads
3. **MQTT** ([ESP01S/mqtt/](ESP01S/mqtt/)) - Publishes decoded sensor data to an MQTT broker

The WiFi implementations support Over-the-Air (OTA) updates.

## Hardware Requirements

- **MCU**: ESP-01S (ESP8266)
- **RF Receiver**: 433 MHz RX module
- **Power**: GPIO3 controls receiver enable
- **Data**: GPIO2 receives RF signals
- **Sensors**: Compatible Auriol 433 MHz weather sensors

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) installed
- Auriol 433 MHz weather sensors
- WiFi network (for UDP/MQTT implementations)

### Serial Demonstration

Send packets out the ESP8266 serial port.

```bash
cd ESP01S/udp
pio run -t upload
```

### UDP Implementation

Broadcasts decoded sensor readings as JSON over UDP:

```bash
cd ESP01S/udp
pio run -t upload
```

Receives messages on UDP port 4242 with format:
```json
{"sensor_id": 12, "temperature": 23.5, "battery": true}
```

### MQTT Implementation

Publishes to the MQTT broker under the prefix `home/auriol_sensor`:

- Sensors with a friendly name mapping: `home/auriol_sensor/<name>` (e.g. `home/auriol_sensor/outside`), published with the retain flag.
- Unmapped sensors: `home/auriol_sensor/ID/<sensor_id>` (4-digit hex, e.g. `home/auriol_sensor/ID/A1B2`), not retained.

Subscribe to `home/auriol_sensor/#` to see everything.

```bash
cd ESP01S/mqtt
pio run -t upload
```

Configure MQTT broker in `secrets.h` before upload.

#### Sensor to Friendly Name Mapping

The MQTT implementation includes a web interface to map sensor IDs to friendly names.

This is especially important when you want to log the sensor data using MQTT listeners,
since the sensor ID is volatile and changes every time the sensor reboots (e.g. after
changing the batter).

After uploading the firmware, open your browser to:

```
http://<device-ip>/
```

This web page allows you to:
- **View** all current sensor mappings
- **Add/Update** mappings by entering sensor ID (hex) and friendly name
- **Delete** mappings you no longer need

Default mappings come pre-configured:
| Sensor ID | Friendly Name |
|-----------|---------------|
| 60        | lounge        |
| 61        | outside       |
| E6        | shed          |

Mappings are persisted in the device's LittleFS storage as `/mappings.csv` and survive firmware reboots.


## Configuration

Create `ESP01S/common/secrets.h`:

```c
#define WIFI_SSID "your_ssid"
#define WIFI_PASSWORD "your_password"
#define MQTT_BROKER "your_broker_ip"
#define MQTT_PORT 1883
```

## Sensor Data Output

All implementations output the same decoded sensor data:

| Field | Type | Range | Example |
|-------|------|-------|---------|
| sensor_id | uint16 | 0-255 | 12 |
| temperature | float | -40 to +60 | 23.5 |
| humidity | uint8 | 0-100 | 65 |
| battery | bool | OK/Low | true |
| channel | uint8 | 1-4 | 1 |

## Project Structure

```
ESP01S/
├── common/           # Shared configuration & secrets
├── demo/             # A serial example.
├── mqtt/             # MQTT publish example
└── udp/              # UDP broadcast example
```

## OTA
Once the WiFi project has been written to the device, future updates can use OTA:

```bash
cd ESP01S/mqtt (or udp)
pio run -t upload --upload-port <Device IP address>
```

## License

Licensed under the GNU General Public License. See LICENSE file for details.

