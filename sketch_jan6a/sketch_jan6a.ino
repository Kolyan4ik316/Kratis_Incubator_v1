#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <ESP32Servo.h> 
#include "KratisNetworkManager.h" 
#include <time.h> 
#include <Preferences.h> 

// --- НОВІ БІБЛІОТЕКИ ДЛЯ OTA ---
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

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

// !!! ТУТ ЗМІНЮЄМО ВЕРСІЮ ПРИ КОЖНОМУ ОНОВЛЕННІ !!!
#define FW_VERSION "1.0.1" 

char uniqueDeviceId[32]; 

const char* AP_SSID = "Kratis-Incubator-01";
const char* AP_PASS = "12345678";

// --- ПІНИ ESP32-C3 SUPER MINI ---
#define I2C_SDA 5
#define I2C_SCL 6
#define BOOT_BUTTON_PIN 9
#define DHTPIN 2      
#define DHTTYPE DHT22 
#define SERVO_PIN 0   

// --- НАЛАШТУВАННЯ МОСФЕТА ---
#define HEATER_PIN 8      // Пін для Мосфета нагрівача
#define FAN_PIN 3      // Пін для Мосфета вентилятора

DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Об'єкти
Servo incubatorServo;
KratisNetworkManager* network = nullptr; 
Preferences preferences; // Основний об'єкт пам'яті

unsigned long lastSensorRead = 0;
unsigned long buttonPressStart = 0;
unsigned long lastTimeSave = 0; // Таймер для збереження часу

float temp = 0.0;
float hum = 0.0;
String lastCmd = "Start";

// --- ЗМІННІ ДЛЯ СЕРВО ---
int currentServoAngle = 0; 
int targetServoAngle = 0;  
const int stepDelay = 60; 

// Отримання часу (рядок)
String getFormattedTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "--:--";
    }
    if (timeinfo.tm_year < 120) return "--:--";

    char timeStringBuff[16];
    // Формат: HH:MM DD.MM
    strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M %d.%m", &timeinfo);
    return String(timeStringBuff);
}

// Функція збереження поточного часу (Timestamp) в пам'ять
void saveCurrentTime() {
    time_t now;
    time(&now);
    if (now > 1600000000) { // Перевірка, чи час валідний (більше 2020 року)
        preferences.putULong("saved_time", (unsigned long)now);
        LOGF("[MEMORY] Time Saved: %lu\n", (unsigned long)now);
    }
}

// --- CALLBACK ФУНКЦІЇ ДЛЯ OTA ---
void update_started() {
  LOG("[OTA] Callback: Update process STARTED");
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "OTA UPDATE...");
  u8g2.sendBuffer();
}

void update_finished() {
  LOG("[OTA] Callback: Update process FINISHED");
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "UPDATE DONE!");
  u8g2.drawStr(0, 20, "Rebooting...");
  u8g2.sendBuffer();
}

void update_progress(int cur, int total) {
  static int lastPercent = -1;
  int percent = (cur * 100) / total;
  if (percent != lastPercent && percent % 10 == 0) { // Логуємо кожні 10%
    LOGF("[OTA] Progress: %d%%\n", percent);
    lastPercent = percent;
    
    // Можна виводити на екран, але це сповільнить процес
    u8g2.setCursor(0, 30);
    u8g2.print(percent); u8g2.print("%");
    u8g2.sendBuffer();
  }
}

void update_error(int err) {
  LOGF("[OTA] Callback: ERROR code %d\n", err);
}

// --- ФУНКЦІЯ ОНОВЛЕННЯ ПРОШИВКИ ---
void performUpdate(String url) {
    if (url.length() < 10) {
        LOG("[OTA] Error: Invalid URL");
        return;
    }

    LOG("\n[OTA] --- STARTING FIRMWARE UPDATE ---");
    LOG("[OTA] URL: " + url);
    LOGF("[OTA] Current Free Heap: %d bytes\n", ESP.getFreeHeap());
    LOGF("[OTA] Free Sketch Space: %d bytes\n", ESP.getFreeSketchSpace());

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "CONNECTING OTA...");
    u8g2.sendBuffer();

    WiFiClientSecure client;
    client.setInsecure(); 
    
    // Підключаємо колбеки для детального логування
    httpUpdate.onStart(update_started);
    httpUpdate.onEnd(update_finished);
    httpUpdate.onProgress(update_progress);
    httpUpdate.onError(update_error);

    // Важливо: ми самі перезавантажимо, якщо все ОК
    httpUpdate.rebootOnUpdate(false); 
    
    LOG("[OTA] Requesting file from server...");
    
    // Запускаємо оновлення
    t_httpUpdate_return ret = httpUpdate.update(client, url);

    // Обробка результату
    switch (ret) {
      case HTTP_UPDATE_FAILED:
        LOGF("[OTA] FAILED! Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        u8g2.clearBuffer();
        u8g2.drawStr(0, 20, "UPDATE FAILED!");
        u8g2.setCursor(0, 30);
        u8g2.print("Err: "); u8g2.print(httpUpdate.getLastError());
        u8g2.sendBuffer();
        delay(5000);
        break;

      case HTTP_UPDATE_NO_UPDATES:
        LOG("[OTA] Server says: No updates");
        break;

      case HTTP_UPDATE_OK:
        LOG("[OTA] SUCCESS! Firmware written.");
        LOG("[OTA] Restarting system in 3 seconds...");
        u8g2.clearBuffer();
        u8g2.drawStr(0, 20, "SUCCESS!");
        u8g2.drawStr(0, 30, "Rebooting...");
        u8g2.sendBuffer();
        delay(3000);
        ESP.restart();
        break;
    }
}

void onCommandReceived(String cmd, String source) {
    lastCmd = cmd;
    LOGF("[LOG] Command Received: %s via %s\n", cmd.c_str(), source.c_str());
    
    // --- ОНОВЛЕННЯ ПРОШИВКИ (OTA) ---
    // Очікуваний формат: "UPDATE:https://myserver.com/firmware.bin"
    if (cmd.startsWith("UPDATE:")) {
        String url = cmd.substring(7); // Обрізаємо "UPDATE:"
        if (url.length() > 5) {
            performUpdate(url);
        }
    }
    // --- ЛОГІКА СЕРВО ---
    else if (cmd.startsWith("SERVO:")) {
        // 1. Зберігаємо час ПЕРЕД початком руху (вимога)
        saveCurrentTime();
        
        int angle = cmd.substring(6).toInt();
        if (angle < 0) angle = 0;
        if (angle > 180) angle = 180;
        
        LOGF("[ACTUATOR] New Target: %d\n", angle);
        targetServoAngle = angle; 
        
        // Зберігаємо ціль
        preferences.putInt("target", targetServoAngle);
    }
    // --- СИНХРОНІЗАЦІЯ ЧАСУ ---
    else if (cmd.startsWith("SYNC_TIME:")) {
        long unsigned int epoch = strtoul(cmd.substring(10).c_str(), NULL, 10);
        if (epoch > 1600000000) {
            struct timeval tv;
            tv.tv_sec = epoch;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            LOGF("[TIME] Synced: %lu\n", epoch);
            
            // Одразу збережемо свіжий час
            saveCurrentTime();
        }
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
        // Рядок 1: ЧАС
        String header = "";
        
        if (!network->isConnected()) {
             header = "!NO WIFI!"; 
        } else {
             String timeStr = getFormattedTime();
             if (timeStr == "--:--") header = "Loading...";
             else header = timeStr;
        }
        
        u8g2.setCursor(0, 9);
        u8g2.print(header);
        
        // Рядок 2: Температура + Версія (маленька буква v)
        u8g2.setCursor(0, 19); 
        u8g2.print("T:"); u8g2.print(temp, 1);
        u8g2.setCursor(37, 19);
        u8g2.print("v"); u8g2.print(FW_VERSION); // Покажемо версію
        
        // Рядок 3: Вологість + Кут серви
        u8g2.setCursor(0, 29); 
        u8g2.print("H:"); u8g2.print(hum, 0); 
        u8g2.setCursor(35, 29);
        u8g2.print("S:"); u8g2.print(currentServoAngle);
        if(currentServoAngle != targetServoAngle) u8g2.print(">"); 
        
        // Рядок 4: Команда
        u8g2.setCursor(0, 39);
        String shortCmd = lastCmd.length() > 10 ? lastCmd.substring(0, 10) : lastCmd;
        u8g2.print(">" + shortCmd);
    }
    u8g2.sendBuffer();
}

void checkHardwareButton() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (buttonPressStart == 0) {
            buttonPressStart = millis();
        }
        unsigned long duration = millis() - buttonPressStart;

        if (duration > 10000) { 
            u8g2.clearBuffer();
            u8g2.drawStr(0, 20, "RESETTING...");
            u8g2.sendBuffer();
            
            if(network) network->factoryReset();
            preferences.clear(); // Очистка пам'яті
            buttonPressStart = 0; 
        } 
    } else {
        if (buttonPressStart != 0) {
            unsigned long duration = millis() - buttonPressStart;
            if (duration > 3000 && duration < 10000 && network && !network->isApMode()) {
                network->startApMode(AP_SSID, AP_PASS); 
            }
            buttonPressStart = 0;
        }
    }
}

void generateDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uniqueDeviceId, sizeof(uniqueDeviceId), "esp32_%04X%08X", 
             (uint16_t)(mac >> 32), (uint32_t)mac);
    LOGF("[SYSTEM] Generated Device ID: %s\n", uniqueDeviceId);
}

void setup() {
    Serial.begin(115200);
    delay(1000); 
    LOG("\n--- SYSTEM BOOT (ESP32-C3) ---");
    LOGF("Firmware Version: %s\n", FW_VERSION);
    
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
    
    generateDeviceId();

    LOG("Init I2C...");
    Wire.begin(I2C_SDA, I2C_SCL);
    
    LOG("Init DHT & OLED...");
    dht.begin();
    u8g2.begin();
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    
    // --- ПАМ'ЯТЬ ---
    preferences.begin("incubator_data", false);
    
    // 1. Відновлюємо позицію
    currentServoAngle = preferences.getInt("current", 0);
    targetServoAngle = preferences.getInt("target", 0);
    
    // 2. Відновлюємо час
    unsigned long savedTime = preferences.getULong("saved_time", 0);
    if (savedTime > 0) {
        struct timeval tv;
        tv.tv_sec = savedTime;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        LOGF("[MEMORY] Restored Time: %lu\n", savedTime);
    }
    
    LOGF("Memory Read -> Current: %d, Target: %d\n", currentServoAngle, targetServoAngle);

    // --- СЕРВО: Тільки налаштування, БЕЗ write() ---
    LOG("Init Servo Config...");
    ESP32PWM::allocateTimer(0);
    incubatorServo.setPeriodHertz(50); 
    incubatorServo.attach(SERVO_PIN, 500, 2400);

    // --- НАГРІВАЧ (МОСФЕТ) ---
    LOG("Init Heater...");
    // Використовуємо новий API для ESP32 Core 3.x
    ledcAttach(HEATER_PIN, 10000, 8); 
    ledcWrite(HEATER_PIN, 217); // Вмикаємо на 85% (255)
    LOG("Heater ON (Max Power)");

    ledcAttach(FAN_PIN, 10000, 8); 
    ledcWrite(FAN_PIN, 127); // Вмикаємо на 50% (255 максимум)
    LOG("Fan ON (Low Power)");
    
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "System Init...");
    u8g2.drawStr(0, 20, "Heater: ON");
    u8g2.drawStr(0, 30, "Mem: OK");
    u8g2.sendBuffer();
    delay(2000);

    LOG("Configuring Network...");
    network = new KratisNetworkManager(SERVER_URL, uniqueDeviceId);
    network->setDeviceType(DEVICE_TYPE);
    
    // --- НОВЕ: Передаємо версію прошивки ---
    network->setFirmwareVersion(FW_VERSION);
    
    network->setApCredentials(AP_SSID, AP_PASS); 
    network->setCommandCallback(onCommandReceived);
    network->begin();
}

void loop() {
    checkHardwareButton();
    
    // --- ЛОГІКА ЗБЕРЕЖЕННЯ ЧАСУ (КОЖНІ 10 ХВ) ---
    if (millis() - lastTimeSave > 600000) { // 600 000 мс = 10 хв
        saveCurrentTime();
        lastTimeSave = millis();
    }

    // --- ПРІОРИТЕТНА ЛОГІКА РУХУ (BLOCKING) ---
    if (currentServoAngle != targetServoAngle) {
        
        // Рухаємось
        if (currentServoAngle < targetServoAngle) currentServoAngle++;
        else currentServoAngle--;
        
        // Ось тут ми подаємо сигнал на сервопривід
        incubatorServo.write(currentServoAngle);
        updateDisplay();
        delay(stepDelay); 
        
        // Зберігаємо позицію кожні 10 градусів
        if (currentServoAngle % 10 == 0 || currentServoAngle == targetServoAngle) {
             preferences.putInt("current", currentServoAngle);
        }
        
        return; 
    }

    // --- ФОНОВІ ЗАДАЧІ ---
    if(network) network->handle();

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