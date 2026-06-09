# Auriol 433MHz Sensor Decoder

This project decodes RF transmissions from Auriol weather sensors operating at 433 MHz. It supports two sensor types with distinct payload formats and encoding schemes.

## System Architecture

- **MCU**: ESP-01S (ESP8266)
- **RF Receiver**: 433 MHz RX module
- **Power**: GPIO3 (RX_POWER_PIN) controls receiver enable
- **Data**: GPIO2 (RF_DATA_PIN) receives pulse-width modulated signals
- **Transport**: WiFi + UDP broadcast to port 4242
- **Output Format**: JSON (ArduinoJson)

## RF Physical Layer

### Modulation & Timing

Both sensor types use **pulse-width modulation (PWM)** with duration-based symbol classification:
- **Encoding**: On-Off Keying (OOK) with variable pulse widths
- **Symbol Detection**: Rising edge capture only; time interval between edges determines bit value
- **Bit 0**: Short pulse (2400 µs Type 1, 1400 µs Type 2)
- **Bit 1**: Long pulse (4400 µs Type 1, 2400 µs Type 2)

| Timing Measurement | Interpretation |
|--------------------|-----------------|
| 600 µs minimum     | Glitch threshold (below = ignored) |
| 15000 µs (15 ms)   | Train reset (packet boundary marker) |

### Profile Detection

The decoder performs **automatic profile detection** by scoring timing differences:

1. First 12-24 symbols establish profile lock
2. Timing comparison between Type 1 and Type 2 standards
3. Lock held until 3 consecutive train resets occur

## Type 1: Temperature-Only Sensors

**Payload**: 32 bits  
**Example Sensors**: Indoor/outdoor thermometer units  
**Frequency**: Periodic transmissions (~9-10 second intervals)

### RF Timing

| Symbol | Duration (µs) | Tolerance |
|--------|---------------|-----------|
| Bit 0  | 2400          | ±480 (20%) |
| Bit 1  | 4400          | ±880 (20%) |
| Gap    | 15000+        | Train reset |

### Payload Structure

```c
uint8_t b0 = (raw32 >> 24) & 0xFF;  // Sensor ID + Battery
uint8_t b1 = (raw32 >> 16) & 0xFF;  // Temperature (mixed) + ???
uint8_t b2 = (raw32 >> 8) & 0xFF;   // Temperature (complete)
uint8_t b3 = raw32 & 0xFF;          // Checksum
```

| Byte | Bits | Assignment | Status |
|------|------|------------|--------|
| b0[7] | Battery flag | Assigned |  |
| b0[6:0] | Sensor ID (7 bits) | Assigned |  |
| **b1[7:4]** | **High nibble** | **UNASSIGNED** | 4 bits unused |
| b1[3:0] | Temperature (upper 4 bits) | Assigned |  |
| b2[7:0] | Temperature (lower 8 bits) | Assigned |  |
| b3[7:0] | Checksum | Assigned (unchecked) |  |

**Unassigned: 4 bits in b1 high nibble** (purpose unknown—possibly reserved or manufacturer-specific)


### Type 1 Examples

| Raw Hex (32-bit) | Sensor | Temp (°C) | Battery | Notes |
|------------------|--------|-----------|---------|-------|
| 0x0C810081       | 0x0C   | 25.6      | OK      | Inside unit |
| 0xE600EB04       | 0xE6   | 23.5      | Low     | Outside unit |
| 0x0CC0FFB4       | 0x0C   | 25.5      | OK      | Reference |
| 0xE600EAF0       | 0xE6   | 23.4      | Low     | Reference |

### Temperature Decoding (Type 1)

```c
int16_t rawTemp = ((int16_t)(b1 & 0x0F) << 8) | b2;
if (rawTemp >= 2048)
    rawTemp -= 4096;  // Two's complement conversion
float finalTemp = rawTemp / 10.0f;
```

## Type 2: Temp/Humidity/Channel Sensors

**Payload**: 36 bits (9 nibbles)  
**Example Sensors**: Multi-channel wireless weather station  
**Frequency**: Periodic transmissions (~9-10 second intervals)

### RF Timing

| Symbol | Duration (µs) | Tolerance |
|--------|---------------|-----------|
| Bit 0  | 1400          | ±280 (20%) |
| Bit 1  | 2400          | ±480 (20%) |
| Gap    | 15000+        | Train reset |

### Payload Structure

```c
uint8_t n0 = (raw36 >> 32) & 0x0F;  // Sensor ID upper
uint8_t n1 = (raw36 >> 28) & 0x0F;  // Sensor ID lower
uint8_t n2 = (raw36 >> 24) & 0x0F;  // Control byte
uint8_t n3 = (raw36 >> 20) & 0x0F;  // Temp upper
uint8_t n4 = (raw36 >> 16) & 0x0F;  // Temp middle
uint8_t n5 = (raw36 >> 12) & 0x0F;  // Temp lower
uint8_t n7 = (raw36 >> 4) & 0x0F;   // Humidity upper
uint8_t n8 = raw36 & 0x0F;          // Humidity lower
// NOTE: n6 is COMPLETELY SKIPPED
```

| Nibble | Bits | Assignment | Status |
|--------|------|------------|--------|
| n0 | Sensor ID upper | Assigned |  |
| n1 | Sensor ID lower | Assigned |  |
| n2[3] | Battery flag | Assigned |  |
| n2[2] | **Unused** | **UNASSIGNED** | 1 bit unknown |
| n2[1:0] | Channel offset (0-2) | Assigned |  |
| n3[3:0] | Temperature upper | Assigned |  |
| n4[3:0] | Temperature middle | Assigned |  |
| n5[3:0] | Temperature lower | Assigned |  |
| **n6[3:0]** | **SKIPPED ENTIRELY** | **UNASSIGNED** | 4 bits not extracted |
| n7[3:0] | Humidity upper | Assigned |  |
| n8[3:0] | Humidity lower | Assigned |  |

**Unassigned: n2 bit 2 (1 bit) + complete n6 nibble (4 bits) = 5 total bits**


### Type 2 Examples

| Raw Hex (36-bit)   | Sensor | Temp (°C) | Humidity (%) | Channel | Battery |
|--------------------|--------|-----------|-------------|---------|---------|
| 0x5790F7F2B        | 0x57   | 24.7      | 43          | 2       | OK      |
| 0x5790FCF2A        | 0x57   | 24.8      | 42          | 1       | Low     |
| 0x7C10F4F2B        | 0xXX   | 24.4      | 43          | 1       | Low     |
| 0x7C90F4F2C        | 0xXX   | 24.4      | 44          | 2       | OK      |

### Temperature Decoding (Type 2)

```c
uint16_t rawTemp = ((uint16_t)n3 << 8) | ((uint16_t)n4 << 4) | n5;
float finalTemp = rawTemp / 10.0f;
```

### Humidity Decoding (Type 2)

```c
uint8_t humidity = (uint8_t)((n7 << 4) | n8);
```

### Channel Decoding (Type 2)

```c
uint8_t channel = (n2 & 0x03) + 1;  // Maps 0-3 to channels 1-4
```

### Battery Decoding (Type 2)

```c
bool batteryOk = (n2 & 0x08) != 0;  // Bit 3 of n2
```

## Unassigned Bits

| Type | Unassigned Bits | Details |
|------|-----------------|---------|
| **Type 1** | 4 bits | b1 high nibble (bits 23-20)—purpose unknown |
| **Type 2** | 5 bits | n2 bit 2 (1 bit, possibly reserved) + n6 nibble (4 bits, completely extracted from payload) |


## Decoder Operation

### State Machine

1. **Profile Lock** (12-24 samples)
   - Accumulates timing error scores for Type 1 vs Type 2
   - Selects profile with lower total error
   - Once locked, remains stable until 3 train resets occur

2. **Packet Assembly**
   - Collects bits between gap markers
   - Validates bit count (26-52 bits accepted)
   - Accumulates consensus count for repeated identical packets

3. **Consensus & Publish**
   - Publishes Auriol payload only after **2nd identical transmission**
   - Duplicate suppression within 1500 ms window
   - Hard bit-length gating:
     - Type 1: exactly 32 bits
     - Type 2: exactly 36 bits

### Validation Rules

| Rule | Type 1 | Type 2 |
|------|--------|--------|
| Bit Length | 32 | 36 |
| Profile Lock Min Samples | 12 | 12 |
| Profile Lock Force Samples | 24 | 24 |
| Consensus Threshold | 2 repeats | 2 repeats |
| Duplicate Suppression | 1500 ms | 1500 ms |
| Glitch Filter | < 600 µs | < 600 µs |


## Protocol Robustness

### Consensus-Based Validation

- All packets must be received **twice identically** to be considered valid
- Prevents single-bit errors from corrupting measurements
- Trade-off: ~20-30 second latency for first packet of session

## Known Limitations

1. **No checksum validation** – All valid bit-length packets are decoded
2. **Type 2 only**: No check digit validation
3. **Consensus latency** – New sensors require 2 identical transmissions
4. **No sensor ID validation** – Arbitrary IDs accepted

## Future Enhancements

- [ ] CRC-8 checksum validation for Type 1 (algorithm TBD)
- [ ] CRC detection for Type 2 payloads
- [ ] Sensor pairing / blacklist mechanism

