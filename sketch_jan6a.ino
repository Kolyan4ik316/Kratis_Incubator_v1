#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <ESP32Servo.h> 
#include "KratisNetworkManager.h" 
#include "KratisQCManager.h"
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

// --- НАЛАШТУВАННЯ ПЕРИФЕРІЇ (МОСФЕТИ ТА РЕЛЕ) ---
#define HEATER_PIN 7      // Пін для Мосфета нагрівача (ШІМ)
#define FAN_PIN 3         // Пін для Мосфета вентилятора (ШІМ)
#define HUMIDIFIER_PIN 4 // Пін для Зволожувача (ON/OFF)

// --- НАЛАШТУВАННЯ QC 3.0 (ДЛЯ РЕГУЛЮВАННЯ НАПРУГИ) ---
// ТЕПЕР ЗГІДНО З РЕАЛЬНОЮ ПАЙКОЮ:
// Пін 10 йде на D+ (через 2.2кОм, і 10кОм до землі)
// Пін 1 йде на D- (через 2.2кОм, і 1кОм до землі)
#define PIN_DP 10
#define PIN_DM 1

DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Об'єкти
Servo incubatorServo;
KratisNetworkManager* network = nullptr; 
KratisQCManager qc(PIN_DP, PIN_DM);
Preferences preferences; 

unsigned long lastSensorRead = 0;
unsigned long buttonPressStart = 0;
unsigned long lastTimeSave = 0; 

float temp = 0.0;
float hum = 0.0;
String lastCmd = "Start";

// --- ЗМІННІ ДЛЯ СЕРВО ---
int currentServoAngle = 0; 
int targetServoAngle = 0;  
const int stepDelay = 60;

float m_current = 5.0;

// Отримання часу
String getFormattedTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) return "--:--";
    if (timeinfo.tm_year < 120) return "--:--";
    char timeStringBuff[16];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M %d.%m", &timeinfo);
    return String(timeStringBuff);
}

void saveCurrentTime() {
    time_t now;
    time(&now);
    if (now > 1600000000) {
        preferences.putULong("saved_time", (unsigned long)now);
        LOGF("[MEMORY] Time Saved: %lu\n", (unsigned long)now);
    }
}

// --- OTA CALLBACKS ---
void update_started() { LOG("[OTA] Update STARTED"); }
void update_finished() { LOG("[OTA] Update FINISHED"); }
void update_progress(int cur, int total) { LOGF("[OTA] Progress: %d%%\n", (cur * 100) / total); }
void update_error(int err) { LOGF("[OTA] ERROR code %d\n", err); }

void performUpdate(String url) {
    if (url.length() < 10) return;
    WiFiClientSecure client;
    client.setInsecure(); 
    httpUpdate.onStart(update_started);
    httpUpdate.onEnd(update_finished);
    httpUpdate.onProgress(update_progress);
    httpUpdate.onError(update_error);
    httpUpdate.rebootOnUpdate(false); 
    t_httpUpdate_return ret = httpUpdate.update(client, url);
    if (ret == HTTP_UPDATE_OK) {
        delay(3000);
        ESP.restart();
    }
}

// --- ОБРОБКА КОМАНД (ВКЛЮЧАЮЧИ КАЛІБРУВАННЯ) ---
void onCommandReceived(String cmd, String source) {
    lastCmd = cmd;
    LOGF("[LOG] Command Received: %s via %s\n", cmd.c_str(), source.c_str());
    
    // OTA Update
    if (cmd.startsWith("UPDATE:")) {
        m_current = 5.0;
        qc.set5V();
        performUpdate(cmd.substring(7));
    }
    
    // --- КАЛІБРУВАННЯ НАГРІВАЧА (0-100%) ---
    else if (cmd.startsWith("CAL_HEAT:")) {
        qc.forceHandshake(); // Завжди робимо рукостискання при зміні на 9V
        m_current = 9.0;
        int power = cmd.substring(9).toInt();
        power = constrain(power, 0, 100);
        //int pwm = map(power, 0, 100, 0, 254);
        int pwm = map(power, 0, 100, 0, 130);
        ledcWrite(HEATER_PIN, pwm);
        LOGF("[CAL] Heater: %d%% (PWM: %d)\n", power, pwm);
    }

    // --- КАЛІБРУВАННЯ ВЕНТИЛЯТОРА (0-100%) ---
    else if (cmd.startsWith("CAL_FAN:")) {
        qc.set9V();
        m_current = 9.0;
        int speed = cmd.substring(8).toInt();
        speed = constrain(speed, 0, 100);
        int pwm = map(speed, 0, 100, 0, 143);
        ledcWrite(FAN_PIN, pwm);
        LOGF("[CAL] Fan: %d%% (PWM: %d)\n", speed, pwm);
    }

    // --- КАЛІБРУВАННЯ СЕРВО (0-180) ---
    else if (cmd.startsWith("CAL_SERVO:") || cmd.startsWith("SERVO:")) {
        saveCurrentTime();
        qc.set5V();
        int offset = cmd.startsWith("CAL_SERVO:") ? 10 : 6;
        int angle = cmd.substring(offset).toInt();
        targetServoAngle = constrain(angle, 0, 180);
        LOGF("[CAL] Servo Target: %d\n", targetServoAngle);
        preferences.putInt("target", targetServoAngle);
    }

    // --- КЕРУВАННЯ ЗВОЛОЖУВАЧЕМ ---
    else if (cmd.startsWith("CAL_HUM:")) {
        qc.set5V();
        m_current = 5.0;
        int state = cmd.substring(8).toInt();
        digitalWrite(HUMIDIFIER_PIN, state == 1 ? HIGH : LOW);
        LOGF("[CAL] Humidifier: %s\n", state == 1 ? "ON" : "OFF");
    }

    // Sync Time
    else if (cmd.startsWith("SYNC_TIME:")) {
        unsigned long epoch = strtoul(cmd.substring(10).c_str(), NULL, 10);
        if (epoch > 1600000000) {
            struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
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
        //u8g2.drawStr(0, 29, "192.168.4.1");
    } else {
        String header = network->isConnected() ? getFormattedTime() : "!NO WIFI!";
        u8g2.setCursor(0, 9); u8g2.print(header);
        u8g2.setCursor(0, 19); u8g2.print("T:"); u8g2.print(temp, 1);
        u8g2.setCursor(37, 19); u8g2.print("v"); u8g2.print(m_current);
        u8g2.setCursor(0, 29); u8g2.print("H:"); u8g2.print(hum, 0); 
        u8g2.setCursor(35, 29); u8g2.print("S:"); u8g2.print(currentServoAngle);
        if(currentServoAngle != targetServoAngle) u8g2.print(">"); 
        u8g2.setCursor(0, 39);
        u8g2.print(">" + (lastCmd.length() > 10 ? lastCmd.substring(0, 10) : lastCmd));
    }
    u8g2.sendBuffer();
}

void checkHardwareButton() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (buttonPressStart == 0) buttonPressStart = millis();
        if (millis() - buttonPressStart > 10000) { 
            network->factoryReset();
            preferences.clear();
            ESP.restart();
        } 
    } else {
        if (buttonPressStart != 0) {
            unsigned long dur = millis() - buttonPressStart;
            if (dur > 3000 && dur < 10000 && !network->isApMode()) network->startApMode(AP_SSID, AP_PASS);
            buttonPressStart = 0;
        }
    }
}

void generateDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uniqueDeviceId, sizeof(uniqueDeviceId), "esp32_%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
}

void setup() {
    Serial.begin(115200);
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1); tzset();
    generateDeviceId();

    Wire.begin(I2C_SDA, I2C_SCL);
    dht.begin();
    u8g2.begin();
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(HUMIDIFIER_PIN, OUTPUT);
    digitalWrite(HUMIDIFIER_PIN, LOW);
    
    preferences.begin("incubator_data", false);
    currentServoAngle = preferences.getInt("current", 0);
    targetServoAngle = preferences.getInt("target", 0);
    
    unsigned long savedTime = preferences.getULong("saved_time", 0);
    if (savedTime > 0) {
        struct timeval tv = { .tv_sec = (time_t)savedTime, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }

    ESP32PWM::allocateTimer(0);
    incubatorServo.setPeriodHertz(50); 
    incubatorServo.attach(SERVO_PIN, 500, 2400);

    // Ініціалізація ШІМ для Нагрівача та Вентилятора
    ledcAttach(HEATER_PIN, 1000, 8); 
    ledcAttach(FAN_PIN, 20000, 8); 
    
    // Стартові значення
    ledcWrite(HEATER_PIN, 0); 
    ledcWrite(FAN_PIN, 0); // 127 = 50%

    network = new KratisNetworkManager(SERVER_URL, uniqueDeviceId);
    network->setDeviceType(DEVICE_TYPE);
    network->setFirmwareVersion(FW_VERSION);
    network->setApCredentials(AP_SSID, AP_PASS); 
    network->setCommandCallback(onCommandReceived);

    // Ініціалізація нашої кастомної логіки QC (робимо ДО запуску мережі та WiFi)
    qc.begin();
    qc.set9V();
    Serial.println("QC 9V sequence completed!");

    network->begin();
}

void loop() {
    checkHardwareButton();
    
    if (millis() - lastTimeSave > 600000) {
        saveCurrentTime();
        lastTimeSave = millis();
    }

    // --- РУХ СЕРВО (БЕЗ ПЕРЕЗАПИСУ ПАМ'ЯТІ В ПРОЦЕСІ) ---
    if (currentServoAngle != targetServoAngle) {
        if (currentServoAngle < targetServoAngle) currentServoAngle++;
        else currentServoAngle--;
        
        incubatorServo.write(currentServoAngle);
        updateDisplay();
        delay(stepDelay); 
        
        // Зберігаємо ТІЛЬКИ в кінці шляху
        if (currentServoAngle == targetServoAngle) {
             preferences.putInt("current", currentServoAngle);
        }
        return; 
    }

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