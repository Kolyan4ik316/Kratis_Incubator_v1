#include "KratisNetworkManager.h"

// Додамо макрос для логування всередині класу
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

void KratisNetworkManager::begin() {
    LOG_NM("Starting...");
    _preferences.begin("config", true); // Відкриваємо тільки для читання
    _ownerId = _preferences.getString("owner", "");
    int count = _preferences.getInt("wifi_count", 0);
    _preferences.end(); // Обов'язково закриваємо

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

    _lastLocalActivity = millis() - LOCAL_TIMEOUT - 1000;
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

    // WiFiMulti.run() повертає статус підключення
    uint8_t status = _wifiMulti.run();

    if (status == WL_CONNECTED) {
        if (!_isServerStarted) {
            _server->begin();
            _isServerStarted = true;
            LOGF_NM("WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            pollCloudServer();
        }
        _server->handleClient();
    } else {
        if (_isServerStarted) {
            LOG_NM("WiFi Lost!");
            _isServerStarted = false;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (millis() - _lastLocalActivity > LOCAL_TIMEOUT) {
            if (!_cloudModeActive) {
                _cloudModeActive = true;
                LOG_NM("Switched to CLOUD MODE (Local timeout)");
            }
            if (millis() - _lastPollTime > CLOUD_POLL_INTERVAL) {
                _lastPollTime = millis();
                pollCloudServer();
            }
        }
    }
}

void KratisNetworkManager::handleLocalRequest() {
    if (_cloudModeActive) {
        LOG_NM("Switched to LAN MODE (Request received)");
    }
    _lastLocalActivity = millis();
    _cloudModeActive = false;

    if (_server->hasArg("val") && _onCommand) {
        _onCommand(_server->arg("val"), "LAN");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["type"] = _deviceType;
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
    http.setTimeout(3000);
    String url = String(_serverUrl) + "/api/sync";
    
    JsonDocument doc;
    doc["id"] = _deviceId;
    doc["type"] = _deviceType;
    doc["local_ip"] = WiFi.localIP().toString();
    
    JsonObject data = doc["data"].to<JsonObject>();
    data["temp"] = _temp;
    data["hum"] = _hum;
    
    String jsonBody;
    serializeJson(doc, jsonBody);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(jsonBody);
    
    if (code == 200) {
        String resp = http.getString();
        JsonDocument respDoc;
        DeserializationError error = deserializeJson(respDoc, resp);
        
        if (!error && !respDoc["cmd"].isNull() && _onCommand) {
            _onCommand(respDoc["cmd"].as<String>(), "CLOUD");
        }
    } else {
        LOGF_NM("Cloud Error: %d\n", code);
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

                // ВАЖЛИВО: Повертаємо реальний device_id телефону!
                JsonDocument respDoc;
                respDoc["status"] = "saved";
                respDoc["device_id"] = _deviceId; // Це ID з MAC-адреси
                
                String respStr;
                serializeJson(respDoc, respStr);

                _server->send(200, "application/json", respStr);
                
                delay(1000);
                ESP.restart();
                return;
            }
        }
    }
    _server->send(400, "application/json", "{\"status\":\"error\"}");
}

// ВИПРАВЛЕНО: Надійне скидання
void KratisNetworkManager::factoryReset() {
    LOG_NM("FACTORY RESET executing...");
    _preferences.begin("config", false); // Відкриваємо для запису
    
    // 1. Явно видаляємо лічильник мереж
    _preferences.remove("wifi_count"); 
    
    // 2. На всяк випадок чистимо все
    _preferences.clear();
    
    _preferences.end(); // Закриваємо і комітимо зміни
    
    LOG_NM("Memory cleared. Restarting in 2s...");
    delay(2000); // Даємо більше часу флеш-пам'яті
    ESP.restart();
}

bool KratisNetworkManager::isApMode() { return _isApMode; }
bool KratisNetworkManager::isCloudActive() { return _cloudModeActive; }
bool KratisNetworkManager::isConnected() { return WiFi.status() == WL_CONNECTED; }
String KratisNetworkManager::getLastIp() { return WiFi.localIP().toString(); }