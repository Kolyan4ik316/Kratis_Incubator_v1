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

// Q.C. 3.0
#include "KratisQCManager.h"

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
#define I2C_SDA         5
#define I2C_SCL         6
#define BOOT_BUTTON_PIN 9
#define DHTPIN          2
#define DHTTYPE         DHT22
#define HEATER_PIN      7
#define FAN_PIN         3
#define HUMIDIFIER_PIN  4
#define SERVO_PIN       0
#define PIN_DP          10
#define PIN_DM          1

DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Об'єкти
Servo incubatorServo;
KratisNetworkManager* network = nullptr; 
Preferences preferences; // Основний об'єкт пам'яті
KratisQCManager qc(PIN_DP, PIN_DM);

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

enum VoltageState { VOLT_5, VOLT_9 };
VoltageState voltageState = VOLT_5;
static int max_9v_heater = 165;
static int max_5v_heater = 254;

static int max_9v_fan = 110;
static int max_5v_fan = 255;

int heaterMaxPWM() { 
    int max_val = voltageState == VOLT_9 ? max_9v_heater : max_5v_heater; 
    // Зменшуємо максимальну відкриваємість мосфета нагрівача на 25%, 
    // коли працює зволожувач (щоб залишити струм для котушки)
    if (humidifierOn) max_val = max_val * 0.75;
    return max_val;
}
int fanMaxPWM()    { return voltageState == VOLT_9 ?  max_9v_fan : max_5v_fan; }

float m_current = 5.0;

bool humidifierOn      = false;
bool humidifierPending = false;

int currentHeaterDuty = 0;
int currentFanDuty    = 0;

static bool grid_mode_hum = false;

// --- ЗАСТОСУВАННЯ ПОТУЖНОСТІ НАГРІВАЧА ---
void applyHeaterPower() {
    int max_allowed = heaterMaxPWM(); // Враховує пониження на 25% коли працює зволожувач
    int actual = currentHeaterDuty;
    if (actual > max_allowed) {
        actual = max_allowed; // фізично обрізаємо до безпечної межі
    }
    ledcWrite(HEATER_PIN, actual);
}

// --- PID РЕГУЛЯТОР ТЕМПЕРАТУРИ ---
bool pidEnabled = false;        // Увімкнути/вимкнути авторегулювання
float targetTemperature = 37.7; // Цільова температура для інкубації

// --- АВТОМАТИЧНА ВОЛОГІСТЬ ---
bool autoHumEnabled = false;    // Увімкнути/вимкнути авторегулювання вологості
float targetHumidity = 50.0;    // Цільова вологість
int autoHumState = 0;           // 0: IDLE, 1: ON, 2: COOLDOWN
unsigned long autoHumTimer = 0;

// Коефіцієнти (потребуватимуть налаштування на реальному залізі)
float Kp = 50.0;  // Пропорційний
float Ki = 0.5;   // Інтегральний (дуже малий для інерційних систем)
float Kd = 10.0;  // Диференціальний

float integralSum = 0.0;
float lastError = 0.0;
unsigned long lastPidTime = 0;

void scaleAndApplyDuties(int hMaxOld, int hMaxNew, int fMaxOld, int fMaxNew) {
    int newHeater = constrain((int)((long)currentHeaterDuty * hMaxNew / hMaxOld), 0, hMaxNew);
    int newFan    = constrain((int)((long)currentFanDuty    * fMaxNew / fMaxOld), 0, fMaxNew);
    LOGF("[POWER] Scale heat %d*(%d/%d)->%d, fan %d*(%d/%d)->%d\n",
         currentHeaterDuty, hMaxNew, hMaxOld, newHeater,
         currentFanDuty,    fMaxNew, fMaxOld, newFan);
    currentHeaterDuty = newHeater;
    currentFanDuty    = newFan;
    applyHeaterPower();
    ledcWrite(FAN_PIN,    currentFanDuty);
}

void enableGridMode(bool ignore_save = false) {
    LOG("[POWER] Switching to GRID MODE (9V)...");
    // 1. Cut-off — зупиняємо все перед перемиканням
    ledcWrite(HEATER_PIN, 0);
    ledcWrite(FAN_PIN, 0);
    // 2. QC хендшейк (всередині затримки 1500+1500ms) + перемикання на 9V
    qc.forceHandshake();
    // 3. Очікуємо стабілізації напруги
    delay(500);
    // 4. Тепер актуальна напруга 9V — масштабуємо duties під нові ліміти і вмикаємо
    scaleAndApplyDuties(max_5v_heater, max_9v_heater, max_5v_fan, max_9v_fan);
    voltageState = VOLT_9;
    m_current = 9.0;
    if(!ignore_save)
        preferences.putBool("grid_mode", true);
    LOG("[POWER] Grid ON (9V)");
}

void disableGridMode(bool ignore_save = false) {
    LOG("[POWER] Switching to BATTERY MODE (5V)...");
    // 1. Cut-off — зупиняємо все перед перемиканням
    ledcWrite(HEATER_PIN, 0);
    ledcWrite(FAN_PIN, 0);
    // 2. Скидаємо QC на 5V
    qc.set5V();
    // 3. Очікуємо стабілізації напруги (QC реагує за ~200ms)
    delay(500);
    // 4. Тепер актуальна напруга 5V — масштабуємо duties під нові ліміти і вмикаємо
    scaleAndApplyDuties(max_9v_heater, max_5v_heater, max_9v_fan, max_5v_fan);
    voltageState = VOLT_5;
    m_current = 5.0;
    if(!ignore_save)
        preferences.putBool("grid_mode", false);
    LOG("[POWER] Battery ON (5V)");
}


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
    else if (cmd.startsWith("SET_POWER_MODE:")) {
        String mode = cmd.substring(15);
        if (mode == "grid") {
            enableGridMode();
        } else if (mode == "battery") {
            disableGridMode();
        }
    }
    else if (cmd.startsWith("CAL_HEAT:")) {
        int pct = constrain(cmd.substring(9).toInt(), 0, 100);
        currentHeaterDuty = map(pct, 0, 100, 0, heaterMaxPWM());
        applyHeaterPower();
        LOGF("[CAL] Heater: %d%% -> duty %d (max %d @ %sV)\n",
             pct, currentHeaterDuty, heaterMaxPWM(), voltageState == VOLT_9 ? "9" : "5");
    }
    else if (cmd.startsWith("CAL_FAN:")) {
        int pct        = constrain(cmd.substring(8).toInt(), 0, 100);
        currentFanDuty = map(pct, 0, 100, 0, fanMaxPWM());
        ledcWrite(FAN_PIN, currentFanDuty);
        LOGF("[CAL] Fan: %d%% -> duty %d (max %d @ %sV)\n",
             pct, currentFanDuty, fanMaxPWM(), voltageState == VOLT_9 ? "9" : "5");
    }
     else if (cmd.startsWith("CAL_HUM:")) {
        //int state = cmd.substring(8).toInt();
        /*if (state == 1) {
            humidifierOn = true;
            if (voltageState == VOLT_9) {
                humidifierPending = true;
                qc.set5V();
                voltageState = VOLT_5;
                m_current = 5.0;
                LOG("[HUM] Waiting for 5V to enable humidifier...");
            } else {
                digitalWrite(HUMIDIFIER_PIN, HIGH);
                LOG("[HUM] ON");
            }
        } else {
            humidifierOn      = false;
            humidifierPending = false;
            digitalWrite(HUMIDIFIER_PIN, LOW);
            LOG("[HUM] OFF");
            if (preferences.getBool("grid_mode", false)) {
                enableGridMode();
            }
        }*/
        // Вимикаємо авто-режим, якщо користувач натиснув ручне керування
        if (autoHumEnabled) {
            autoHumEnabled = false;
            preferences.putBool("hum_enabled", false);
            LOG("[AUTO_HUM] Disabled due to manual override.");
        }
        
        int state = cmd.substring(8).toInt();
        if (state == 1)
        {
            if(!humidifierOn)
            {
                if(preferences.getBool("grid_mode", false))
                {
                    grid_mode_hum = true;
                    disableGridMode(true);
                }
                humidifierOn = true;
                digitalWrite(HUMIDIFIER_PIN, HIGH);
                applyHeaterPower(); // Одразу знижуємо максимум нагрівача
                LOG("[HUM] ON");
            }
        }
        else
        {
            if (humidifierOn) {
                humidifierOn = false;
                digitalWrite(HUMIDIFIER_PIN, LOW);
                applyHeaterPower(); // Повертаємо нормальний максимум нагрівача
                LOG("[HUM] OFF");
                
                if(grid_mode_hum)
                {
                    grid_mode_hum = false;
                    enableGridMode(true);
                }
            }
        }
    }
    // --- НАЛАШТУВАННЯ PID ---
    else if (cmd.startsWith("PID_EN:")) {
        int state = cmd.substring(7).toInt();
        pidEnabled = (state == 1);
        preferences.putBool("pid_enabled", pidEnabled);
        
        // Якщо виключили PID, скидаємо нагрівач і вентилятор (або залишаємо як є - краще вимкнути, щоб не перегрілось)
        if (!pidEnabled) {
            ledcWrite(HEATER_PIN, 0);
            currentHeaterDuty = 0;
            // Якщо необхідно також вимкнути вентилятор: ledcWrite(FAN_PIN, 0);
            LOG("[PID] Disabled manually. Heater off.");
        } else {
            LOG("[PID] Enabled manually.");
            lastPidTime = millis(); // Скидання таймера PID
        }
    }
    else if (cmd.startsWith("PID_TEMP:")) {
        float newTemp = cmd.substring(9).toFloat();
        if (newTemp >= 20.0 && newTemp <= 45.0) { // Обмеження безпечної температури
            targetTemperature = newTemp;
            preferences.putFloat("target_temp", targetTemperature);
            LOGF("[PID] Target Temp changed to: %.1f\n", targetTemperature);
        } else {
            LOGF("[PID] Invalid temp: %.1f (Safe range 20-45)\n", newTemp);
        }
    }
    // --- НАЛАШТУВАННЯ АВТО ВОЛОГОСТІ ---
    else if (cmd.startsWith("HUM_EN:")) {
        int state = cmd.substring(7).toInt();
        autoHumEnabled = (state == 1);
        preferences.putBool("hum_enabled", autoHumEnabled);
        
        LOGF("[AUTO_HUM] Mode set to: %d\n", autoHumEnabled);
    }
    else if (cmd.startsWith("HUM_TARGET:")) {
        float newHum = cmd.substring(11).toFloat();
        if (newHum >= 10.0 && newHum <= 95.0) { // Межі
            targetHumidity = newHum;
            preferences.putFloat("target_hum", targetHumidity);
            LOGF("[AUTO_HUM] Target Humidity changed to: %.1f%%\n", targetHumidity);
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
        u8g2.setCursor(50, 19);
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
    
    // 3. Відновлюємо налаштування PID та вологості
    pidEnabled = preferences.getBool("pid_enabled", false); // За замовчуванням вимкнено (або true, як захочете)
    targetTemperature = preferences.getFloat("target_temp", 37.7);
    
    autoHumEnabled = preferences.getBool("hum_enabled", false);
    targetHumidity = preferences.getFloat("target_hum", 50.0);
    
    LOGF("Memory Read -> Current: %d, Target: %d\n", currentServoAngle, targetServoAngle);
    LOGF("Memory Read -> PID Enabled: %d, Target Temp: %.1f\n", pidEnabled, targetTemperature);
    LOGF("Memory Read -> Hum Enabled: %d, Target Hum: %.1f\n", autoHumEnabled, targetHumidity);

    // --- СЕРВО: Тільки налаштування, БЕЗ write() ---
    LOG("Init Servo Config...");
    ESP32PWM::allocateTimer(0);
    incubatorServo.setPeriodHertz(50); 
    incubatorServo.attach(SERVO_PIN, 500, 2400);

    // --- НАГРІВАЧ (МОСФЕТ) ---
    LOG("Init Heater, HUM AND Fan...");
    ledcAttach(HEATER_PIN, 1000, 8);
    ledcAttach(FAN_PIN, 20000, 8);
    ledcWrite(HEATER_PIN, 0);
    ledcWrite(FAN_PIN, 0);
    pinMode(HUMIDIFIER_PIN, OUTPUT);
    digitalWrite(HUMIDIFIER_PIN, LOW);
    LOG("Initialized Heater, HUM AND Fan!!!");

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "System Init...");
    u8g2.drawStr(0, 20, "Heater: ON");
    u8g2.drawStr(0, 30, "Mem: OK");
    u8g2.sendBuffer();
    delay(2000);

    qc.begin();

    LOG("Configuring Network...");
    network = new KratisNetworkManager(SERVER_URL, uniqueDeviceId);
    network->setDeviceType(DEVICE_TYPE);
    
    // --- НОВЕ: Передаємо версію прошивки ---
    network->setFirmwareVersion(FW_VERSION);
    
    network->setApCredentials(AP_SSID, AP_PASS); 
    network->setCommandCallback(onCommandReceived);
    network->begin();

    if (preferences.getBool("grid_mode", false)) {
        LOG("[SETUP] Restoring saved GRID MODE (9V)...");
        enableGridMode();
    } else {
        LOG("[SETUP] BATTERY MODE (5V). Ready.");
    }
}

void computePID() {
    if (!pidEnabled) return;

    unsigned long now = millis();
    float dt = (now - lastPidTime) / 1000.0; // Час в секундах (приблизно 2 сек)
    if (dt <= 0.0) return;
    lastPidTime = now;

    float error = targetTemperature - temp;
    
    // 1. Пропорційна складова
    float P = Kp * error;

    // 2. Інтегральна складова з обмеженням (Anti-Windup)
    // Накопичуємо інтеграл тільки якщо ми близько до цілі (наприклад в межах +-2 градусів)
    if (abs(error) < 2.0) {
        integralSum += error * dt;
    } else {
        integralSum = 0; // Скидаємо, якщо ми далеко, щоб уникнути перегріву
    }
    float I = Ki * integralSum;

    // Обмеження інтегральної суми
    float maxOut = heaterMaxPWM(); // Ваша поточна максимальна потужність (5V або 9V)
    if (I > maxOut) I = maxOut;
    else if (I < 0) I = 0;

    // 3. Диференціальна складова
    float derivative = (error - lastError) / dt;
    float D = Kd * derivative;
    lastError = error;

    // 4. Загальний результат
    float output = P + I + D;

    // 5. Ограничення результату в межах PWM
    int dutyCycle = (int)output;
    if (dutyCycle > maxOut) dutyCycle = maxOut;
    else if (dutyCycle < 0) dutyCycle = 0;

    // Застосовуємо потужність
    currentHeaterDuty = dutyCycle;
    applyHeaterPower();
    
    LOGF("[PID] Err: %.2f | P: %.1f, I: %.1f, D: %.1f | OUT: %d/%d\n", 
         error, P, I, D, currentHeaterDuty, (int)maxOut);
}

void computeAutoHumidity() {
    if (!autoHumEnabled) {
        // Якщо вимкнули авто-режим, а воно лишилося висіти в якомусь стані
        if (autoHumState != 0) {
            digitalWrite(HUMIDIFIER_PIN, LOW);
            autoHumState = 0;
            if (humidifierOn) {
                humidifierOn = false;
                applyHeaterPower();
            }
            // Повертаємо 9V, якщо були там
            if (grid_mode_hum) {
                grid_mode_hum = false;
                enableGridMode(true);
            }
        }
        return;
    }

    switch (autoHumState) {
        case 0: // IDLE - чекаємо поки впаде вологість
            if (hum > 0 && hum < targetHumidity) {
                // Вмикаємо алгоритм накачування вологості
                // Перевіряємо чи треба збити напругу до 5V
                if (voltageState == VOLT_9 || preferences.getBool("grid_mode", false)) {
                    grid_mode_hum = true;
                    disableGridMode(true);
                }
                
                digitalWrite(HUMIDIFIER_PIN, HIGH);
                humidifierOn = true;
                applyHeaterPower(); // Застосовуємо ліміт -25%
                autoHumState = 1; // Перехід до стану ON
                autoHumTimer = millis();
                LOGF("[AUTO_HUM] Started. Target: %.1f, Cur: %.1f\n", targetHumidity, hum);
            }
            break;

        case 1: // ON - працює до 17 секунд або до досягнення цілі +7%
            if (hum >= (targetHumidity + 7.0)) {
                // Досягли цілі достроково
                digitalWrite(HUMIDIFIER_PIN, LOW);
                humidifierOn = false;
                applyHeaterPower(); // Повертаємо 100%
                autoHumState = 0; 
                // Повертаємо 9V, якщо вимикали
                if (grid_mode_hum) {
                    grid_mode_hum = false;
                    enableGridMode(true);
                }
                LOG("[AUTO_HUM] Reached Target + 7% early. Stopping.");
            } 
            else if (millis() - autoHumTimer >= 17000) {
                // Відпрацював 17 секунд, треба відпочивати
                digitalWrite(HUMIDIFIER_PIN, LOW);
                humidifierOn = false;
                applyHeaterPower(); // Повертаємо 100%
                autoHumState = 2; // Перехід до COOLDOWN
                autoHumTimer = millis();
                LOG("[AUTO_HUM] 17s max reached. Cooldown 20s started.");
            }
            break;

        case 2: // COOLDOWN - відпочиває мінімум 20 секунд (котушка остигає)
            // При цьому він залишається в режимі 5V, ми не стрибаємо туди-назад
            if (millis() - autoHumTimer >= 20000) {
                // Перевіряємо, чи набралась вологість за час відпочинку
                if (hum >= (targetHumidity + 7.0)) {
                    autoHumState = 0; // Повертаємось в IDLE
                    // Тепер можна повернути напругу
                    if (grid_mode_hum) {
                        grid_mode_hum = false;
                        enableGridMode(true);
                    }
                    LOG("[AUTO_HUM] Done. Condition met after cooldown.");
                } else {
                    // Вологості все ще не вистачає, робимо наступний цикл
                    digitalWrite(HUMIDIFIER_PIN, HIGH);
                    humidifierOn = true;
                    applyHeaterPower(); // Знову ліміт -25%
                    autoHumState = 1; // Перехід до ON
                    autoHumTimer = millis();
                    LOG("[AUTO_HUM] Cooldown done. Resuming pump.");
                }
            }
            break;
    }
}

void loop() {
    checkHardwareButton();

    if (humidifierPending && voltageState == VOLT_5) {
        humidifierPending = false;
        digitalWrite(HUMIDIFIER_PIN, HIGH);
        LOG("[HUM] ON (voltage settled to 5V)");
    }
    
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

    // Обслуговування авто-вологості
    computeAutoHumidity();

    if (millis() - lastSensorRead > 2000) {
        lastSensorRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t)) 
            temp = t;

        if (!isnan(h)) 
            hum = h;

        // Виклик PID після кожного успішного отримання температури
        if (lastPidTime == 0) lastPidTime = millis(); // ініціалізація
        computePID(); 

        if(network) network->updateSensorData(temp, hum);
        updateDisplay();
    }
}

