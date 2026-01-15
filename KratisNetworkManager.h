#ifndef KRATISNETWORKMANAGER_H
#define KRATISNETWORKMANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

typedef std::function<void(String, String)> CommandCallback;

class KratisNetworkManager {
public:
    KratisNetworkManager(const char* serverUrl, const char* deviceId);
    ~KratisNetworkManager(); // Деструктор для очищення пам'яті

    void begin();
    void handle(); 
    
    void updateSensorData(float temp, float hum);

    void startApMode();
    void factoryReset();
    
    bool isConnected();
    bool isApMode();
    bool isCloudActive();
    String getLastIp();

    void setCommandCallback(CommandCallback callback);

private:
    const char* _serverUrl;
    const char* _deviceId;
    
    WiFiMulti _wifiMulti;
    WebServer* _server; // ВКАЗІВНИК! Вирішує проблему з копіюванням
    Preferences _preferences;

    bool _isApMode = false;
    bool _cloudModeActive = true;
    bool _isServerStarted = false;
    
    float _temp = 0.0;
    float _hum = 0.0;
    String _ownerId = "";

    unsigned long _lastPollTime = 0;
    unsigned long _lastLocalActivity = 0;
    const unsigned long LOCAL_TIMEOUT = 10000;
    const unsigned long CLOUD_POLL_INTERVAL = 3000;

    CommandCallback _onCommand;

    void handleSetupStatus();
    void handleSaveConfig();
    void handleLocalRequest();
    void pollCloudServer();
    void loadNetworks();
};

#endif