#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include "KratisNetworkManager.h"
#include "KratisQCManager.h"
#include <time.h>
#include <Preferences.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

#define DEBUG 1
#if DEBUG
  #define LOG(x) Serial.println("[LOG] " + String(x))
  #define LOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(x)
  #define LOGF(...)
#endif

const char* SERVER_URL   = "https://kratis-p2p-server.onrender.com";
const char* DEVICE_TYPE  = "incubator_v1";
#define FW_VERSION "1.0.4"

char uniqueDeviceId[32];
const char* AP_SSID = "Kratis-Incubator-01";
const char* AP_PASS = "12345678";

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


enum VoltageState { VOLT_5, VOLT_9 };
VoltageState voltageState = VOLT_5;

static int max_9v_heater = 165;
static int max_5v_heater = 254;
static int max_9v_fan    = 110;
static int max_5v_fan    = 255;

int heaterMaxPWM() { return voltageState == VOLT_9 ? max_9v_heater : max_5v_heater; }
int fanMaxPWM()    { return voltageState == VOLT_9 ? max_9v_fan : max_5v_fan; }


DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Servo incubatorServo;
KratisNetworkManager* network = nullptr;
KratisQCManager qc(PIN_DP, PIN_DM);
Preferences preferences;


float temp = 0.0;
float hum  = 0.0;
float m_current = 5.0;
String lastCmd = "Start";

bool humidifierOn      = false;
bool humidifierPending = false;

int currentHeaterDuty = 0;
int currentFanDuty    = 0;

// --- PID РЕГУЛЯТОР ТЕМПЕРАТУРИ ---
bool pidEnabled = false;        // Увімкнути/вимкнути авторегулювання
float targetTemperature = 37.7; // Цільова температура для інкубації

// Коефіцієнти (потребуватимуть налаштування на реальному залізі)
float Kp = 55.0;  // Пропорційний (більший імпульс при помилці 3-4°C)
float Ki = 1.05;  // Інтегральний (швидше накопичення)
float Kd = 20.0;   // Диференціальний (гасить overshoot)

float integralSum = 0.0;
float lastError = 0.0;
unsigned long lastPidTime = 0;

// --- АВТОМАТИЧНА ВОЛОГІСТЬ ---
bool autoHumEnabled = false;    // Увімкнути/вимкнути авторегулювання вологості
float targetHumidity = 50.0;    // Цільова вологість
int autoHumState = 0;           // 0: IDLE, 1: ON, 2: COOLDOWN
unsigned long autoHumTimer = 0;



void scaleAndApplyDuties(int hMaxOld, int hMaxNew, int fMaxOld, int fMaxNew) {
    int newHeater = constrain((int)((long)currentHeaterDuty * hMaxNew / hMaxOld), 0, hMaxNew);
    int newFan    = constrain((int)((long)currentFanDuty    * fMaxNew / fMaxOld), 0, fMaxNew);
    LOGF("[POWER] Scale heat %d*(%d/%d)->%d, fan %d*(%d/%d)->%d\n",
         currentHeaterDuty, hMaxNew, hMaxOld, newHeater,
         currentFanDuty,    fMaxNew, fMaxOld, newFan);
    currentHeaterDuty = newHeater;
    currentFanDuty    = newFan;
    ledcWrite(HEATER_PIN, currentHeaterDuty);
    ledcWrite(FAN_PIN,    currentFanDuty);
}

unsigned long lastSensorRead  = 0;
unsigned long buttonPressStart = 0;
unsigned long lastTimeSave    = 0;

int currentServoAngle = 0;
int targetServoAngle  = 0;
const int stepDelay = 60;
static bool grid_mode_hum = false;


void nonBlockingDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        if (network) network->handle();
        delay(5); // коротка пауза, щоб не вантажити CPU
    }
}

void enableGridMode(bool ignore_save = false) {
    if (voltageState == VOLT_9) {
        LOG("[POWER] Already in GRID MODE (9V), skipping.");
        return;
    }
    LOG("[POWER] Switching to GRID MODE (9V)...");
    ledcWrite(HEATER_PIN, 0);
    ledcWrite(FAN_PIN, max_9v_fan);
    qc.forceHandshake();
    nonBlockingDelay(500);
    scaleAndApplyDuties(max_5v_heater, max_9v_heater, max_5v_fan, max_9v_fan);
    voltageState = VOLT_9;
    m_current = 9.0;
    if(!ignore_save)
        preferences.putBool("grid_mode", true);
    LOG("[POWER] Grid ON (9V)");
}

void disableGridMode(bool ignore_save = false) {
    if (voltageState == VOLT_5) {
        LOG("[POWER] Already in BATTERY MODE (5V), skipping.");
        return;
    }
    LOG("[POWER] Switching to BATTERY MODE (5V)...");
    ledcWrite(HEATER_PIN, 0);
    ledcWrite(FAN_PIN, 0);
    qc.set5V();
    nonBlockingDelay(500);
    scaleAndApplyDuties(max_9v_heater, max_5v_heater, max_9v_fan, max_5v_fan);
    voltageState = VOLT_5;
    m_current = 5.0;
    if(!ignore_save)
        preferences.putBool("grid_mode", false);
    LOG("[POWER] Battery ON (5V)");
}


String getFormattedTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "--:--";
    if (timeinfo.tm_year < 120)  return "--:--";
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M %d.%m", &timeinfo);
    return String(buf);
}

void saveCurrentTime() {
    time_t now; time(&now);
    if (now > 1600000000) {
        preferences.putULong("saved_time", (unsigned long)now);
        LOGF("[MEMORY] Time saved: %lu\n", (unsigned long)now);
    }
}


void update_started()  { LOG("[OTA] Update STARTED"); }
void update_finished() { LOG("[OTA] Update FINISHED"); }
void update_progress(int cur, int total) { LOGF("[OTA] Progress: %d%%\n", (cur * 100) / total); }
void update_error(int err) { LOGF("[OTA] ERROR: %d\n", err); }

void performUpdate(String url) {
    if (url.length() < 10) return;
    if (voltageState != VOLT_5) disableGridMode();
    WiFiClientSecure client;
    client.setInsecure();
    httpUpdate.onStart(update_started);
    httpUpdate.onEnd(update_finished);
    httpUpdate.onProgress(update_progress);
    httpUpdate.onError(update_error);
    httpUpdate.rebootOnUpdate(false);
    t_httpUpdate_return ret = httpUpdate.update(client, url);
    if (ret == HTTP_UPDATE_OK) { delay(3000); ESP.restart(); }
}

void onCommandReceived(String cmd, String source) {
    lastCmd = cmd;
    LOGF("[CMD] %s via %s\n", cmd.c_str(), source.c_str());

    if (cmd.startsWith("UPDATE:")) {
        performUpdate(cmd.substring(7));
    }
    else if (cmd.startsWith("SET_POWER_MODE:")) {
        String mode = cmd.substring(15);
        if (mode == "grid") {
            // Повністю скидаємо всі стани зволожувача та авто-режиму
            if (humidifierOn) {
                digitalWrite(HUMIDIFIER_PIN, LOW);
                humidifierOn = false;
                LOG("[CMD] Humidifier forced OFF for GRID MODE");
            }
            if (humidifierPending) {
                humidifierPending = false;
                LOG("[CMD] humidifierPending reset for GRID MODE");
            }
            if (grid_mode_hum) {
                grid_mode_hum = false;
                LOG("[CMD] grid_mode_hum reset for GRID MODE");
            }
            autoHumState = 0;
            LOG("[CMD] autoHumState reset for GRID MODE");
            enableGridMode();
            nonBlockingDelay(2000);
        } else if (mode == "battery") {
            disableGridMode();
        }
    }

    else if (cmd.startsWith("CAL_HEAT:")) {
        int pct           = constrain(cmd.substring(9).toInt(), 0, 100);
        currentHeaterDuty = map(pct, 0, 100, 0, heaterMaxPWM());
        ledcWrite(HEATER_PIN, currentHeaterDuty);
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
    else if (cmd.startsWith("CAL_HUM:")) {
        if (autoHumEnabled) {
            autoHumEnabled = false;
            preferences.putBool("hum_enabled", false);
            LOG("[AUTO_HUM] Disabled due to manual override.");
        }
        int state = cmd.substring(8).toInt();
        if (state == 1) {
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
                enableGridMode(true);
            }
        }
    }

    else if (cmd.startsWith("CAL_SERVO:") || cmd.startsWith("SERVO:")) {
        saveCurrentTime();
        int offset = cmd.startsWith("CAL_SERVO:") ? 10 : 6;
        int angle = constrain(cmd.substring(offset).toInt(), 0, 180);
        targetServoAngle = angle;
        LOGF("[SERVO] Target: %d\n", targetServoAngle);
        preferences.putInt("target", targetServoAngle);
    }

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

    if (!network) {
        u8g2.drawStr(0, 9, "NO NET OBJ");
        u8g2.sendBuffer();
        return;
    }

    if (network->isApMode()) {
        u8g2.drawStr(0, 9, "SETUP MODE");
        u8g2.drawStr(0, 19, AP_SSID); 
        u8g2.drawStr(0, 29, "192.168.4.1");
    } else {
        String header = network->isConnected() ? getFormattedTime() : "!NO WIFI!";
        u8g2.setCursor(0, 9);  u8g2.print(header);
        u8g2.setFont(u8g2_font_8x13B_tf);
        u8g2.setCursor(0, 20); u8g2.print("T:"); u8g2.print(temp, 1);
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.setCursor(45,20); u8g2.print(voltageState == VOLT_9 ? " v9" : " v5");
        u8g2.setFont(u8g2_font_8x13B_tf);
        u8g2.setCursor(0, 30); u8g2.print("H:"); u8g2.print(hum, 0);
        //u8g2.setCursor(35,29); u8g2.print("S:"); u8g2.print(currentServoAngle);
        //if (currentServoAngle != targetServoAngle) u8g2.print(">");
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.setCursor(0, 40);
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
            if (dur > 3000 && dur < 10000 && !network->isApMode())
                network->startApMode(AP_SSID, AP_PASS);
            buttonPressStart = 0;
        }
    }
}


void generateDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uniqueDeviceId, sizeof(uniqueDeviceId), "esp32_%04X%08lX",
             (uint16_t)(mac >> 32), (uint32_t)mac);
}

void setup() {
    Serial.begin(115200);
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1); tzset();
    generateDeviceId();

    Wire.begin(I2C_SDA, I2C_SCL);
    dht.begin();
    u8g2.begin();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    u8g2.drawStr(0, 9, "LOADING START");
    u8g2.sendBuffer();
    ledcAttach(FAN_PIN,    20000, 8);
    ledcWrite(FAN_PIN,    255);
    delay(500);


    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(HUMIDIFIER_PIN, OUTPUT);
    digitalWrite(HUMIDIFIER_PIN, LOW);

    preferences.begin("incubator_data", false);
    currentServoAngle = preferences.getInt("current", 0);
    targetServoAngle  = preferences.getInt("target",  0);

         // 3. Відновлюємо налаштування PID
    pidEnabled = preferences.getBool("pid_enabled", false); // За замовчуванням вимкнено (або true, як захочете)
    targetTemperature = preferences.getFloat("target_temp", 37.7);

    autoHumEnabled = preferences.getBool("hum_enabled", false);
    targetHumidity = preferences.getFloat("target_hum", 50.0);
    
    LOGF("Memory Read -> Current: %d, Target: %d\n", currentServoAngle, targetServoAngle);
    LOGF("Memory Read -> PID Enabled: %d, Target Temp: %.1f\n", pidEnabled, targetTemperature);
    LOGF("Memory Read -> Hum Enabled: %d, Target Hum: %.1f\n", autoHumEnabled, targetHumidity);

    unsigned long savedTime = preferences.getULong("saved_time", 0);
    if (savedTime > 0) {
        struct timeval tv = { .tv_sec = (time_t)savedTime, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }

    ESP32PWM::allocateTimer(0);
    incubatorServo.setPeriodHertz(50);
    incubatorServo.attach(SERVO_PIN, 500, 2400);

    ledcAttach(HEATER_PIN, 1000,  8);
    ledcWrite(HEATER_PIN, 0);
    //ledcWrite(FAN_PIN,    0);
    qc.begin();

    u8g2.drawStr(0, 9, "LOADING START");
    if (preferences.getBool("grid_mode", false)) {
        LOG("[SETUP] Restoring saved GRID MODE (9V)...");
        u8g2.drawStr(0, 18, "9V GRID");
        enableGridMode();
        
        
    } else {
        LOG("[SETUP] BATTERY MODE (5V). Ready.");
        u8g2.drawStr(0, 18, "5V BATTERY");
    }
    u8g2.sendBuffer();
    delay(2000);
    u8g2.drawStr(0, 9, "LOADING START");
    if(voltageState == VOLT_9)
        u8g2.drawStr(0, 18, "9V GRID");
    else
        u8g2.drawStr(0, 18, "5V BATTERY");

    u8g2.drawStr(0, 35, "SUCCESSFUL");
    u8g2.sendBuffer();

    network = new KratisNetworkManager(SERVER_URL, uniqueDeviceId);
    network->setDeviceType(DEVICE_TYPE);
    network->setFirmwareVersion(FW_VERSION);
    network->setApCredentials(AP_SSID, AP_PASS);
    network->setCommandCallback(onCommandReceived);
    network->begin();
    
}

void computePID() {
    if (!pidEnabled) return;

    unsigned long now = millis();
    float dt = (now - lastPidTime) / 1000.0; 
    if (dt <= 0.0 || dt > 10.0) { // Захист від першого запуску або збою часу
        lastPidTime = now;
        return;
    }
    lastPidTime = now;

    float error = targetTemperature - temp;
    
    // --- КОЕФІЦІЄНТИ З СИМУЛЯЦІЇ ---
    // Kp = 55.0, Ki = 0.06, Kd = 260.0
    
    // 1. Пропорційна складова
    float P = Kp * error;

    if (temp > (targetTemperature + 0.15)) {
        currentHeaterDuty = 0;
        integralSum = 0; // Обов'язково чистимо пам'ять інтеграла
        ledcWrite(HEATER_PIN, 0);
        
        // Вентилятор залишаємо на максимум, щоб швидше розігнати застій тепла
        currentFanDuty = fanMaxPWM();
        ledcWrite(FAN_PIN, currentFanDuty);
        
        LOG("[PID] OVERHEAT! Safety Cut-off active.");
        return; 
    }

    // 2. Інтегральна складова (Anti-Windup + Вузьке вікно)
    // Зменшуємо вікно до 1.0 градуса, щоб не накопичувати зайвого тепла
    if (abs(error) < 1.0) {
        integralSum += error * dt;
    } else {
        // Замість повного скидання — плавне згасання, 
        // щоб прибрати ривок при вході в зону регулювання
        integralSum *= 0.9; 
    }

    // Обмеження інтеграла (щоб він не міг самостійно видати більше max PWM)
    float maxOut = (float)heaterMaxPWM();
    if (Ki > 0) {
        integralSum = constrain(integralSum, 0, maxOut / Ki);
    }
    float I = Ki * integralSum;

    // 3. Диференціальна складова (Гальмо)
    // Вона має бути сильною, щоб компенсувати інерцію кераміки
    float derivative = (error - lastError) / dt;
    float D = Kd * derivative;
    lastError = error;

    // 4. Загальний результат
    float output = P + I + D;

    // 5. Soft Start / Safety Max
    // Якщо температура дуже низька, не даємо нагрівачу жарити на 100% (255) 
    // при 9В живленні, щоб не перегріти саму кераміку
    int dutyCycle = (int)output;
    dutyCycle = constrain(dutyCycle, 0, (int)maxOut);

    // Застосовуємо потужність
    currentHeaterDuty = dutyCycle;
    ledcWrite(HEATER_PIN, currentHeaterDuty);

    // Вентилятор на повну для перемішування повітря (важливо для зменшення інерції)
    currentFanDuty = fanMaxPWM();
    ledcWrite(FAN_PIN, currentFanDuty);
    
    LOGF("[PID] T:%.2f | Err:%.2f | P:%.1f I:%.1f D:%.1f | PWM:%d\n", 
          temp, error, P, I, D, currentHeaterDuty);
}

void computeAutoHumidity() {
    if (!autoHumEnabled) {
        // Якщо вимкнули авто-режим, а воно лишилося висіти в якомусь стані
        if (autoHumState != 0) {
            digitalWrite(HUMIDIFIER_PIN, LOW);
            autoHumState = 0;
            humidifierOn = false;
            // Повертаємо 9V, якщо були там
            if (grid_mode_hum) {
                grid_mode_hum = false;
                enableGridMode();
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
                        enableGridMode();
                    }
                    LOG("[AUTO_HUM] Done. Condition met after cooldown.");
                } else {
                    // Вологості все ще не вистачає, робимо наступний цикл
                    digitalWrite(HUMIDIFIER_PIN, HIGH);
                    humidifierOn = true;
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

    if (millis() - lastTimeSave > 600000) {
        saveCurrentTime();
        lastTimeSave = millis();
    }

    if (humidifierPending && voltageState == VOLT_5) {
        LOG("[HUM] humidifierPending resolved, enabling humidifier");
        humidifierPending = false;
        digitalWrite(HUMIDIFIER_PIN, HIGH);
        LOG("[HUM] ON (voltage settled to 5V)");
    }

    if (currentServoAngle != targetServoAngle) {
        if (currentServoAngle < targetServoAngle) currentServoAngle++;
        else                                      currentServoAngle--;
        incubatorServo.write(currentServoAngle);
        updateDisplay();
        delay(stepDelay);
        if (currentServoAngle == targetServoAngle)
            preferences.putInt("current", currentServoAngle);
        return;
    }

    if (network) network->handle();

    if (millis() - lastSensorRead > 2000) {
        lastSensorRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t)) temp = t;
        if (!isnan(h)) hum  = h;
        if (network) network->updateSensorData(temp, hum);
        updateDisplay();

        // Викликаємо PID і авто-зволоження тільки після оновлення сенсорів!
        if (lastPidTime == 0) lastPidTime = millis();
        computePID();
        computeAutoHumidity();
    }
}