#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include "KratisNetworkManager.h" 

// --- DEBUG CONFIG ---
#define DEBUG 1 

#if DEBUG
  #define LOG(x) Serial.println("[LOG] " + String(x))
  #define LOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(x)
  #define LOGF(...)
#endif

// --- КОНФІГУРАЦІЯ ---
const char* SERVER_URL = "https://kratis-p2p-server.onrender.com"; 
const char* DEVICE_TYPE = "incubator_v1";

// Буфер для динамічного ID
char uniqueDeviceId[32];

const char* AP_SSID = "Kratis-Incubator-01";
const char* AP_PASS = "12345678";

#define I2C_SDA 5
#define I2C_SCL 6
#define BOOT_BUTTON_PIN 9
#define DHTPIN 2      
#define DHTTYPE DHT22 

DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Вказівник на менеджер (ініціалізуємо в setup)
KratisNetworkManager* network = nullptr;

unsigned long lastSensorRead = 0;
unsigned long buttonPressStart = 0;
float temp = 0.0;
float hum = 0.0;
String lastCmd = "Start";

void onCommandReceived(String cmd, String source) {
    lastCmd = cmd;
    LOGF("[LOG] Command Received: %s via %s\n", cmd.c_str(), source.c_str());
    
    if (cmd.startsWith("SERVO:")) {
        int angle = cmd.substring(6).toInt();
        LOGF("[LOG] Moving Servo to %d\n", angle);
    }
}

void updateDisplay() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    if (network->isApMode()) {
        u8g2.drawStr(0, 9, "SETUP MODE");
        u8g2.drawStr(0, 19, AP_SSID); 
        u8g2.drawStr(0, 29, "192.168.4.1");
    } else {
        if (!network->isConnected()) {
            u8g2.drawStr(0, 9, ".NO WIFI.");
        } else if (network->isCloudActive()) {
            u8g2.drawStr(0, 9, "CLOUD MODE");
        } else {
            u8g2.drawStr(0, 9, "LAN MODE");
        }
        
        u8g2.drawStr(0, 19, "T:"); u8g2.setCursor(20, 19); u8g2.print(temp, 1);
        u8g2.drawStr(0, 29, "H:"); u8g2.setCursor(20, 29); u8g2.print(hum, 1);
        
        u8g2.setCursor(0, 39);
        u8g2.print(">" + lastCmd);
    }
    u8g2.sendBuffer();
}

void checkHardwareButton() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (buttonPressStart == 0) {
            buttonPressStart = millis();
            LOG("Button Pressed...");
        }
        
        unsigned long duration = millis() - buttonPressStart;

        if (duration > 10000) { 
            LOG("Button hold > 10s. Triggering FACTORY RESET!");
            u8g2.clearBuffer();
            u8g2.drawStr(0, 20, "RESETTING...");
            u8g2.sendBuffer();
            
            if(network) network->factoryReset();
            buttonPressStart = 0; 
            
        } 
    } else {
        if (buttonPressStart != 0) {
            unsigned long duration = millis() - buttonPressStart;
            LOGF("[LOG] Button Released. Duration: %lu ms\n", duration);
            
            if (duration > 3000 && duration < 10000 && network && !network->isApMode()) {
                LOG("Triggering AP MODE (3-10s hold)");
                network->startApMode(AP_SSID, AP_PASS); 
            }
            buttonPressStart = 0;
        }
    }
}

// Генерація унікального ID з MAC-адреси
void generateDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uniqueDeviceId, sizeof(uniqueDeviceId), "esp32_%04X%08X", 
             (uint16_t)(mac >> 32), (uint32_t)mac);
    LOGF("[SYSTEM] Generated Device ID: %s\n", uniqueDeviceId);
}

void setup() {
    Serial.begin(115200);
    delay(1000); 
    LOG("\n--- SYSTEM BOOT ---");
    
    // 1. Генерація ID
    generateDeviceId();

    LOG("Init I2C...");
    Wire.begin(I2C_SDA, I2C_SCL);
    
    LOG("Init DHT & OLED...");
    dht.begin();
    u8g2.begin();
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "System Init...");
    
    // Покажемо ID на екрані при старті (корисно)
    u8g2.setCursor(0, 30);
    u8g2.print("ID: ");
    u8g2.print(uniqueDeviceId);
    u8g2.sendBuffer();
    delay(2000);

    LOG("Configuring Network Manager...");
    
    // 2. Створюємо менеджер з унікальним ID
    network = new KratisNetworkManager(SERVER_URL, uniqueDeviceId);
    
    network->setDeviceType(DEVICE_TYPE);
    network->setApCredentials(AP_SSID, AP_PASS); 
    network->setCommandCallback(onCommandReceived);
    
    LOG("Starting Network Manager...");
    network->begin();
    LOG("Setup Complete. Entering Loop.");
}

void loop() {
    checkHardwareButton();
    if(network) network->handle(); // Перевірка на nullptr

    if (millis() - lastSensorRead > 2000) {
        lastSensorRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t)) temp = t;
        if (!isnan(h)) hum = h;
        
        if(network) network->updateSensorData(temp, hum);
        updateDisplay();
    }
}