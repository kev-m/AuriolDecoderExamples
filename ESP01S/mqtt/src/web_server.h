#ifndef __web_server_h__
#define __web_server_h__

#include <ESP8266WebServer.h>
#include "sensor_mappings.h"

extern SensorMappings sensorMappings;

// Defined in auriol_mqtt.cpp: clears a persisted (retained) friendly-name topic
// by publishing an empty retained payload to <prefix>/<name>.
extern void clearRetainedTopic(const char* name);

ESP8266WebServer webServer(80);

const char html_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <title>Auriol Sensor Mappings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
        .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        h1 { color: #333; }
        table { width: 100%; border-collapse: collapse; margin: 20px 0; }
        th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }
        th { background: #4CAF50; color: white; }
        tr:hover { background: #f5f5f5; }
        .form-group { margin: 20px 0; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: #333; }
        input { padding: 8px; width: 100%; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; }
        button { background: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; margin-right: 5px; }
        button:hover { background: #45a049; }
        .delete-btn { background: #f44336; }
        .delete-btn:hover { background: #da190b; }
        .reboot-btn { background: #ff9800; }
        .reboot-btn:hover { background: #e68a00; }
        .status { padding: 10px; margin: 10px 0; border-radius: 4px; display: none; }
        .status.success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
        .status.error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Auriol Sensor Mappings</h1>
        <p>Configure friendly names for your sensors.</p>

        <div class="form-group">
            <h2>Add/Update Mapping</h2>
            <label for="sensorId">Sensor ID (hex):</label>
            <input type="text" id="sensorId" placeholder="e.g., 60, 61, E6">
            <label for="sensorName" style="margin-top: 10px;">Friendly Name:</label>
            <input type="text" id="sensorName" placeholder="e.g., lounge, outside">
            <label for="sensorOffset" style="margin-top: 10px;">Offset (&deg;C):</label>
            <input type="number" step="any" id="sensorOffset" placeholder="0.0">
            <label for="sensorScale" style="margin-top: 10px;">Scale:</label>
            <input type="number" step="any" id="sensorScale" placeholder="1.0">
            <p style="color:#666;font-size:0.85em;margin:8px 0;">Correction: T = scale &times; raw + offset. Leave as 0 / 1 to do nothing.</p>
            <button onclick="addMapping()">Add/Update</button>
        </div>

        <div id="status" class="status"></div>

        <h2>Current Mappings</h2>
        <table id="mappingsTable">
            <thead>
                <tr>
                    <th>Sensor ID</th>
                    <th>Friendly Name</th>
                    <th>Offset</th>
                    <th>Scale</th>
                    <th>Action</th>
                </tr>
            </thead>
            <tbody>
            </tbody>
        </table>

        <h2>Device</h2>
        <button class="reboot-btn" onclick="rebootDevice()">Reboot</button>
    </div>

    <script>
        function loadMappings() {
            fetch('/api/mappings')
                .then(r => r.json())
                .then(data => {
                    const tbody = document.querySelector('#mappingsTable tbody');
                    tbody.innerHTML = '';
                    data.mappings.forEach(m => {
                        const offset = (m.offset !== undefined ? m.offset : 0);
                        const scale = (m.scale !== undefined ? m.scale : 1);
                        const row = document.createElement('tr');
                        row.innerHTML = '<td>' + m.sensor_id + '</td><td>' + m.name +
                                        '</td><td>' + offset.toFixed(3) +
                                        '</td><td>' + scale.toFixed(3) +
                                        '</td><td><button onclick="editMapping(\'' + m.sensor_id +
                                        '\',\'' + m.name + '\',' + offset + ',' + scale + ')">Edit</button>' +
                                        '<button class="delete-btn" onclick="deleteMapping(\'' +
                                        m.sensor_id + '\')">Delete</button></td>';
                        tbody.appendChild(row);
                    });
                })
                .catch(e => showStatus('Error loading mappings', 'error'));
        }

        function editMapping(id, name, offset, scale) {
            document.getElementById('sensorId').value = id;
            document.getElementById('sensorName').value = name;
            document.getElementById('sensorOffset').value = offset;
            document.getElementById('sensorScale').value = scale;
            window.scrollTo(0, 0);
        }

        function addMapping() {
            const id = document.getElementById('sensorId').value.trim();
            const name = document.getElementById('sensorName').value.trim();
            const offsetVal = document.getElementById('sensorOffset').value.trim();
            const scaleVal = document.getElementById('sensorScale').value.trim();

            if (!id || !name) {
                showStatus('Please enter both Sensor ID and Friendly Name', 'error');
                return;
            }

            const offset = offsetVal === '' ? 0 : parseFloat(offsetVal);
            const scale = scaleVal === '' ? 1 : parseFloat(scaleVal);

            const payload = JSON.stringify({sensor_id: id, name: name, offset: offset, scale: scale});
            fetch('/api/mappings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: payload
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    showStatus('Mapping saved successfully', 'success');
                    document.getElementById('sensorId').value = '';
                    document.getElementById('sensorName').value = '';
                    document.getElementById('sensorOffset').value = '';
                    document.getElementById('sensorScale').value = '';
                    loadMappings();
                } else {
                    showStatus(data.error || 'Failed to save mapping', 'error');
                }
            })
            .catch(e => showStatus('Error saving mapping', 'error'));
        }

        function deleteMapping(id) {
            if (!confirm('Delete mapping for sensor ' + id + '?')) return;
            const url = '/api/mappings/' + id;
            fetch(url, {method: 'DELETE'})
                .then(r => r.json())
                .then(data => {
                    if (data.success) {
                        showStatus('Mapping deleted', 'success');
                        loadMappings();
                    } else {
                        showStatus('Failed to delete mapping', 'error');
                    }
                })
                .catch(e => showStatus('Error deleting mapping', 'error'));
        }

        function rebootDevice() {
            if (!confirm('Reboot the device?')) return;
            fetch('/api/reboot', {method: 'POST'})
                .then(r => r.json())
                .then(data => showStatus('Rebooting...', 'success'))
                .catch(e => showStatus('Error sending reboot request', 'error'));
        }

        function showStatus(msg, type) {
            const el = document.getElementById('status');
            el.textContent = msg;
            el.className = 'status ' + type;
            el.style.display = 'block';
            setTimeout(() => el.style.display = 'none', 3000);
        }

        document.addEventListener('DOMContentLoaded', function() {
            loadMappings();
        });
    </script>
</body>
</html>
)=====";

void handleRoot() {
    webServer.send_P(200, PSTR("text/html"), html_page);
}

void handleGetMappings() {
    sensor_mapping_t* mappings;
    uint8_t count;
    sensorMappings.getAllMappings(&mappings, &count);

    String json = "{\"mappings\":[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"sensor_id\":\"";
        json += String(mappings[i].sensor_id, HEX);
        json += "\",\"name\":\"";
        json += mappings[i].name;
        json += "\",\"offset\":";
        json += String(mappings[i].offset, 3);
        json += ",\"scale\":";
        json += String(mappings[i].scale, 3);
        json += "}";
    }
    json += "]}";

    webServer.send(200, "application/json", json);
}

void handlePostMapping() {
    if (!webServer.hasArg("plain")) {
        webServer.send(400, "application/json", "{\"success\":false,\"error\":\"No data\"}");
        return;
    }

    String body = webServer.arg("plain");

    int idStart = body.indexOf("\"sensor_id\":\"") + 13;
    int idEnd = body.indexOf("\"", idStart);
    String idStr = body.substring(idStart, idEnd);

    int nameStart = body.indexOf("\"name\":\"") + 8;
    int nameEnd = body.indexOf("\"", nameStart);
    String nameStr = body.substring(nameStart, nameEnd);

    if (idStr.length() == 0 || nameStr.length() == 0) {
        webServer.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid input\"}");
        return;
    }

    // Optional numeric correction fields; default to identity (offset 0, scale 1).
    float offset = 0.0f;
    float scale = 1.0f;

    int offsetStart = body.indexOf("\"offset\":");
    if (offsetStart >= 0) {
        offsetStart += 9;
        int offsetEnd = body.indexOf(",", offsetStart);
        if (offsetEnd < 0) offsetEnd = body.indexOf("}", offsetStart);
        offset = body.substring(offsetStart, offsetEnd).toFloat();
    }

    int scaleStart = body.indexOf("\"scale\":");
    if (scaleStart >= 0) {
        scaleStart += 8;
        int scaleEnd = body.indexOf(",", scaleStart);
        if (scaleEnd < 0) scaleEnd = body.indexOf("}", scaleStart);
        scale = body.substring(scaleStart, scaleEnd).toFloat();
        if (scale == 0.0f) scale = 1.0f; // Guard against a zeroed-out scale
    }

    uint16_t sensor_id = (uint16_t)strtol(idStr.c_str(), NULL, 16);

    // If this sensor is being renamed, clear the old friendly-name topic so its
    // retained value doesn't linger under the previous name.
    const char* oldName = sensorMappings.getName(sensor_id);
    if (oldName && nameStr != oldName) {
        clearRetainedTopic(oldName);
    }

    sensorMappings.setMapping(sensor_id, nameStr.c_str(), offset, scale);

    webServer.send(200, "application/json", "{\"success\":true}");
}

void handleDeleteMapping() {
    String path = webServer.uri();
    int lastSlash = path.lastIndexOf('/');
    String idStr = path.substring(lastSlash + 1);

    uint16_t sensor_id = (uint16_t)strtol(idStr.c_str(), NULL, 16);

    // Clear the persisted friendly-name topic before removing the mapping,
    // otherwise a stale retained value lingers on the broker forever.
    const char* name = sensorMappings.getName(sensor_id);
    if (name) {
        clearRetainedTopic(name);
    }

    sensorMappings.deleteMapping(sensor_id);

    webServer.send(200, "application/json", "{\"success\":true}");
}

void handleReboot() {
    webServer.send(200, "application/json", "{\"success\":true}");
    webServer.client().flush();
    delay(200);
    ESP.restart();
}

void setupWebServer() {
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/api/mappings", HTTP_GET, handleGetMappings);
    webServer.on("/api/mappings", HTTP_POST, handlePostMapping);
    webServer.on("/api/reboot", HTTP_POST, handleReboot);
    webServer.onNotFound([]() {
        if (webServer.method() == HTTP_DELETE) {
            handleDeleteMapping();
        } else {
            webServer.send(404, "text/plain", "Not Found");
        }
    });

    webServer.begin();
}

void handleWebServer() {
    webServer.handleClient();
}

#endif
