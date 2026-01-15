#include "KratisNetworkManager.h"

KratisNetworkManager::KratisNetworkManager(const char* serverUrl, const char* deviceId) 
    : _serverUrl(serverUrl), _deviceId(deviceId) {
    // Ініціалізуємо сервер як динамічний об'єкт
    _server = new WebServer(80);
}

KratisNetworkManager::~KratisNetworkManager() {
    if (_server) {
        delete _server;
    }
}

void KratisNetworkManager::begin() {
    _preferences.begin("config", true);
    _ownerId = _preferences.getString("owner", "");
    int count = _preferences.getInt("wifi_count", 0);
    _preferences.end();

    if (count == 0) {
        startApMode();
    } else {
        loadNetworks();
        WiFi.mode(WIFI_STA);
        
        // Використовуємо стрілку -> для доступу до методів вказівника
        _server->on("/cmd", HTTP_GET, [this]() { this->handleLocalRequest(); });
        _server->on("/status", HTTP_GET, [this]() { this->handleLocalRequest(); });
        _server->onNotFound([this]() { _server->send(204); });
        
        // Запуск сервера відбувається в handle() після підключення WiFi
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

    _wifiMulti.run();

    if (WiFi.status() == WL_CONNECTED) {
        if (!_isServerStarted) {
            _server->begin();
            _isServerStarted = true;
            pollCloudServer(); // Перший пінг
        }
        _server->handleClient();
    } else {
        _isServerStarted = false;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (millis() - _lastLocalActivity > LOCAL_TIMEOUT) {
            _cloudModeActive = true;
            if (millis() - _lastPollTime > CLOUD_POLL_INTERVAL) {
                _lastPollTime = millis();
                pollCloudServer();
            }
        }
    }
}

void KratisNetworkManager::handleLocalRequest() {
    _lastLocalActivity = millis();
    _cloudModeActive = false;

    if (_server->hasArg("val") && _onCommand) {
        _onCommand(_server->arg("val"), "LAN");
    }

    // ArduinoJson v7: використовуємо JsonDocument замість StaticJsonDocument
    JsonDocument doc;
    doc["status"] = "ok";
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
    doc["local_ip"] = WiFi.localIP().toString();
    
    // Створення вкладеного об'єкта в v7
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
        
        // Перевірка v7: respDoc["cmd"].is<String>() або !isNull()
        if (!error && !respDoc["cmd"].isNull() && _onCommand) {
            _onCommand(respDoc["cmd"].as<String>(), "CLOUD");
        }
    }
    http.end();
}

void KratisNetworkManager::startApMode() {
    _isApMode = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Smart-Incubator", "12345678");
    
    // Перезапускаємо сервер для режиму AP
    if (_server) delete _server;
    _server = new WebServer(80);
    
    _server->on("/", HTTP_GET, [this]() { 
        JsonDocument doc;
        doc["device_id"] = _deviceId;
        doc["type"] = "incubator";
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

                _server->send(200, "application/json", "{\"status\":\"saved\"}");
                delay(1000);
                ESP.restart();
                return;
            }
        }
    }
    _server->send(400, "application/json", "{\"status\":\"error\"}");
}

void KratisNetworkManager::factoryReset() {
    _preferences.begin("config", false);
    _preferences.clear();
    _preferences.end();
    delay(1000);
    ESP.restart();
}

bool KratisNetworkManager::isApMode() { return _isApMode; }
bool KratisNetworkManager::isCloudActive() { return _cloudModeActive; }
bool KratisNetworkManager::isConnected() { return WiFi.status() == WL_CONNECTED; }
String KratisNetworkManager::getLastIp() { return WiFi.localIP().toString(); }