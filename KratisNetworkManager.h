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
    ~KratisNetworkManager();

    // Налаштування назви точки доступу (викликати в setup)
    void setApCredentials(const char* ssid, const char* password);
    void setDeviceType(const char* type);

    void begin();
    void handle(); 
    void updateSensorData(float temp, float hum);

    // Тепер можна передати аргументи, або використати збережені через setApCredentials
    void startApMode(const char* ssid = nullptr, const char* password = nullptr);
    void factoryReset();
    
    bool isConnected();
    bool isApMode();
    bool isCloudActive();
    String getLastIp();

    void setCommandCallback(CommandCallback callback);

private:
    const char* _serverUrl;
    const char* _deviceId;
    String _deviceType = "unknown";
    
    // Налаштування для AP Mode
    String _apSsid = "Kratis-Setup";
    String _apPass = "12345678";
    
    WiFiMulti _wifiMulti;
    WebServer* _server; 
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