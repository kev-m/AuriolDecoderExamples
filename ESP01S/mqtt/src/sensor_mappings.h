#ifndef __sensor_mappings_h__
#define __sensor_mappings_h__

#include <FS.h>
#include <LittleFS.h>
#include <map>

#define MAPPINGS_FILE "/mappings.csv"
#define MAX_MAPPINGS 20
#define MAX_NAME_LENGTH 32

typedef struct {
    uint16_t sensor_id;
    char name[MAX_NAME_LENGTH];
    float offset; // Temperature correction: T_corrected = scale * T_raw + offset
    float scale;  // Default identity: offset = 0.0, scale = 1.0 (do nothing)
} sensor_mapping_t;

class SensorMappings {
private:
    sensor_mapping_t mappings[MAX_MAPPINGS];
    uint8_t mapping_count = 0;

    void createDefaultMappings() {
        const char* defaults[] = {
            "60,lounge",
            "61,outside",
            "E6,shed"
        };

        for (int i = 0; i < 3; i++) {
            parseLine(defaults[i]);
        }

        saveMappings();
    }

    void parseLine(const char* line) {
        if (mapping_count >= MAX_MAPPINGS) return;

        char temp[MAX_NAME_LENGTH + 40];
        strncpy(temp, line, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';

        // Fields: id,name[,offset,scale]  (offset/scale optional for backward compat)
        char* comma = strchr(temp, ',');
        if (!comma) return;
        *comma = '\0';
        char* id_str = temp;
        char* name_str = comma + 1;

        // Correction defaults to identity (do nothing) when fields are absent.
        float offset = 0.0f;
        float scale = 1.0f;

        char* offset_str = strchr(name_str, ',');
        if (offset_str) {
            *offset_str = '\0';
            offset_str++;
            char* scale_str = strchr(offset_str, ',');
            if (scale_str) {
                *scale_str = '\0';
                scale_str++;
                scale = (float)atof(scale_str);
            }
            offset = (float)atof(offset_str);
        }

        uint16_t id = (uint16_t)strtol(id_str, NULL, 16);

        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].sensor_id == id) {
                strncpy(mappings[i].name, name_str, MAX_NAME_LENGTH - 1);
                mappings[i].name[MAX_NAME_LENGTH - 1] = '\0';
                mappings[i].offset = offset;
                mappings[i].scale = scale;
                return;
            }
        }

        mappings[mapping_count].sensor_id = id;
        strncpy(mappings[mapping_count].name, name_str, MAX_NAME_LENGTH - 1);
        mappings[mapping_count].name[MAX_NAME_LENGTH - 1] = '\0';
        mappings[mapping_count].offset = offset;
        mappings[mapping_count].scale = scale;
        mapping_count++;
    }

public:
    void init() {
        if (!LittleFS.exists(MAPPINGS_FILE)) {
            createDefaultMappings();
        } else {
            loadMappings();
            // If file exists but is empty, create defaults
            if (mapping_count == 0) {
                createDefaultMappings();
            }
        }
    }

    void loadMappings() {
        mapping_count = 0;
        File file = LittleFS.open(MAPPINGS_FILE, "r");
        if (!file) return;

        while (file.available()) {
            String line = file.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                parseLine(line.c_str());
            }
        }
        file.close();
    }

    void saveMappings() {
        File file = LittleFS.open(MAPPINGS_FILE, "w");
        if (!file) return;

        for (int i = 0; i < mapping_count; i++) {
            file.printf("%02X,%s,%.3f,%.3f\n",
                        mappings[i].sensor_id, mappings[i].name,
                        mappings[i].offset, mappings[i].scale);
        }
        file.close();
    }

    const char* getName(uint16_t sensor_id) {
        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].sensor_id == sensor_id) {
                return mappings[i].name;
            }
        }
        return nullptr;
    }

    // Apply the per-sensor linear correction: T_corrected = scale * T_raw + offset.
    // Unmapped sensors are returned unchanged (identity).
    float applyCorrection(uint16_t sensor_id, float temperature_c) {
        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].sensor_id == sensor_id) {
                return mappings[i].scale * temperature_c + mappings[i].offset;
            }
        }
        return temperature_c;
    }

    void setMapping(uint16_t sensor_id, const char* name, float offset, float scale) {
        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].sensor_id == sensor_id) {
                strncpy(mappings[i].name, name, MAX_NAME_LENGTH - 1);
                mappings[i].name[MAX_NAME_LENGTH - 1] = '\0';
                mappings[i].offset = offset;
                mappings[i].scale = scale;
                saveMappings();
                return;
            }
        }

        if (mapping_count < MAX_MAPPINGS) {
            mappings[mapping_count].sensor_id = sensor_id;
            strncpy(mappings[mapping_count].name, name, MAX_NAME_LENGTH - 1);
            mappings[mapping_count].name[MAX_NAME_LENGTH - 1] = '\0';
            mappings[mapping_count].offset = offset;
            mappings[mapping_count].scale = scale;
            mapping_count++;
            saveMappings();
        }
    }

    void deleteMapping(uint16_t sensor_id) {
        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].sensor_id == sensor_id) {
                for (int j = i; j < mapping_count - 1; j++) {
                    mappings[j] = mappings[j + 1];
                }
                mapping_count--;
                saveMappings();
                return;
            }
        }
    }

    void getAllMappings(sensor_mapping_t** out, uint8_t* count) {
        *out = mappings;
        *count = mapping_count;
    }

    uint8_t getCount() {
        return mapping_count;
    }
};

#endif
