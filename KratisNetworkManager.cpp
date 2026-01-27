#include "KratisNetworkManager.h"

// Макроси логування
#define LOG_NM(x) Serial.println("[NET] " + String(x))
#define LOGF_NM(format, ...) Serial.printf("[NET] " format, __VA_ARGS__)

KratisNetworkManager::KratisNetworkManager(const char* serverUrl, const char* deviceId) 
    : _serverUrl(serverUrl), _deviceId(deviceId) {
    _server = new WebServer(80);
}

KratisNetworkManager::~KratisNetworkManager() {
    if (_server) delete _server;
}

void KratisNetworkManager::setApCredentials(const char* ssid, const char* password) {
    _apSsid = String(ssid);
    if (password) _apPass = String(password);
    else _apPass = "";
}

void KratisNetworkManager::setDeviceType(const char* type) {
    _deviceType = String(type);
}

// Реалізація встановлення версії
void KratisNetworkManager::setFirmwareVersion(const char* version) {
    _fwVersion = String(version);
    LOGF_NM("Firmware Version set to: %s\n", _fwVersion.c_str());
}

void KratisNetworkManager::begin() {
    LOG_NM("Starting...");
    _preferences.begin("config", true); 
    _ownerId = _preferences.getString("owner", "");
    int count = _preferences.getInt("wifi_count", 0);
    _preferences.end(); 

    LOGF_NM("Saved Networks: %d\n", count);

    if (count == 0) {
        LOG_NM("No networks found. Starting AP Mode.");
        startApMode();
    } else {
        LOG_NM("Loading networks...");
        loadNetworks();
        WiFi.mode(WIFI_STA);
        
        _server->on("/cmd", HTTP_GET, [this]() { this->handleLocalRequest(); });
        _server->on("/status", HTTP_GET, [this]() { this->handleLocalRequest(); });
        _server->onNotFound([this]() { _server->send(204); });
        
        LOG_NM("STA Mode started. Waiting for connection...");
    }
    
    _lastLocalActivity = millis();
    _lastPollTime = millis();
}

void KratisNetworkManager::loadNetworks() {
    _preferences.begin("config", true);
    int count = _preferences.getInt("wifi_count", 0);
    
    for (int i = 0; i < count; i++) {
        String key_s = "ssid_" + String(i);
        String key_p = "pass_" + String(i);
        String s = _preferences.getString(key_s.c_str(), "");
        String p = _preferences.getString(key_p.c_str(), "");
        
        if (s.length() > 0) {
            _wifiMulti.addAP(s.c_str(), p.c_str());
            LOGF_NM("Added WiFi: %s\n", s.c_str());
        }
    }
    _preferences.end();
}

void KratisNetworkManager::updateSensorData(float temp, float hum) {
    _temp = temp;
    _hum = hum;
}

void KratisNetworkManager::setCommandCallback(CommandCallback callback) {
    _onCommand = callback;
}

void KratisNetworkManager::handle() {
    if (_isApMode) {
        _server->handleClient();
        return;
    }

    uint8_t status = _wifiMulti.run();

    if (status == WL_CONNECTED) {
        if (!_isServerStarted) {
            _server->begin();
            _isServerStarted = true;
            LOGF_NM("WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            pollCloudServer();
        }
        
        _server->handleClient();

        if (millis() - _lastPollTime > CLOUD_POLL_INTERVAL) {
            _lastPollTime = millis();
            pollCloudServer();
            _cloudModeActive = true; 
        }
    } else {
        if (_isServerStarted) {
            LOG_NM("WiFi Lost!");
            _isServerStarted = false;
        }
    }
}

void KratisNetworkManager::handleLocalRequest() {
    _lastLocalActivity = millis();

    if (_server->hasArg("val") && _onCommand) {
        _onCommand(_server->arg("val"), "LAN");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["type"] = _deviceType;
    doc["fw_ver"] = _fwVersion; 
    doc["temp"] = _temp;
    doc["hum"] = _hum;
    doc["owner"] = _ownerId;
    
    String response;
    serializeJson(doc, response);
    _server->sendHeader("Access-Control-Allow-Origin", "*");
    _server->send(200, "application/json", response);
}

void KratisNetworkManager::pollCloudServer() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    http.setTimeout(10000); // Збільшено таймаут до 10с для надійності OTA
    String url = String(_serverUrl) + "/api/sync";
    
    JsonDocument doc;
    doc["id"] = _deviceId;
    doc["type"] = _deviceType;
    doc["fw_version"] = _fwVersion; 
    doc["local_ip"] = WiFi.localIP().toString();
    
    JsonObject data = doc["data"].to<JsonObject>();
    data["temp"] = _temp;
    data["hum"] = _hum;
    
    String jsonBody;
    serializeJson(doc, jsonBody);

    // LOGF_NM("Sending Sync (v%s)...", _fwVersion.c_str()); // Debug

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(jsonBody);
    
    if (code == 200) {
        String resp = http.getString();
        JsonDocument respDoc;
        DeserializationError error = deserializeJson(respDoc, resp);
        
        if (!error && !respDoc["cmd"].isNull() && _onCommand) {
            String cmd = respDoc["cmd"].as<String>();
            LOGF_NM("Received Command from Cloud: %s\n", cmd.c_str());
            _onCommand(cmd, "CLOUD");
        }
    } else {
        // LOGF_NM("Sync Failed. HTTP Code: %d\n", code);
    }
    http.end();
}

void KratisNetworkManager::startApMode(const char* ssid, const char* password) {
    _isApMode = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    
    const char* apName = ssid ? ssid : _apSsid.c_str();
    const char* apPass = password ? password : _apPass.c_str();
    
    WiFi.softAP(apName, apPass);
    LOGF_NM("AP Started: %s (IP: 192.168.4.1)\n", apName);
    
    if (_server) delete _server;
    _server = new WebServer(80);
    
    _server->on("/", HTTP_GET, [this]() { 
        JsonDocument doc;
        doc["device_id"] = _deviceId;
        doc["type"] = _deviceType;
        doc["fw_ver"] = _fwVersion;
        doc["status"] = "waiting_config";
        String r; serializeJson(doc, r);
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json", r);
    });
    
    _server->on("/save", HTTP_POST, [this]() { this->handleSaveConfig(); });
    _server->begin();
}

void KratisNetworkManager::handleSaveConfig() {
    if (_server->hasArg("plain")) {
        String body = _server->arg("plain");
        LOG_NM("Config Received: " + body);
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, body);
        
        if (!error) {
            String new_ssid = doc["ssid"];
            String new_pass = doc["pass"];
            String user_id = doc["user_id"];

            if (new_ssid.length() > 0) {
                _preferences.begin("config", false);
                int count = _preferences.getInt("wifi_count", 0);
                
                String k_s = "ssid_" + String(count);
                String k_p = "pass_" + String(count);
                
                _preferences.putString(k_s.c_str(), new_ssid);
                _preferences.putString(k_p.c_str(), new_pass);
                _preferences.putInt("wifi_count", count + 1);
                
                if (user_id.length() > 0) {
                    _preferences.putString("owner", user_id);
                }
                
                _preferences.end();
                
                JsonDocument respDoc;
                respDoc["status"] = "saved";
                respDoc["device_id"] = _deviceId;
                String respStr; serializeJson(respDoc, respStr);

                _server->send(200, "application/json", respStr);
                delay(1000);
                ESP.restart();
                return;
            }
        }
    }
    _server->send(400, "application/json", "{\"status\":\"error\"}");
}

void KratisNetworkManager::factoryReset() {
    LOG_NM("FACTORY RESET executing...");
    _preferences.begin("config", false); 
    _preferences.remove("wifi_count"); 
    _preferences.clear();
    _preferences.end();
    LOG_NM("Memory cleared. Restarting in 2s...");
    delay(2000); 
    ESP.restart();
}

bool KratisNetworkManager::isApMode() { return _isApMode; }
bool KratisNetworkManager::isCloudActive() { return _cloudModeActive; }
bool KratisNetworkManager::isConnected() { return WiFi.status() == WL_CONNECTED; }
String KratisNetworkManager::getLastIp() { return WiFi.localIP().toString(); }