#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <ESP32Servo.h> // БІБЛІОТЕКА ДЛЯ СЕРВО
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

char uniqueDeviceId[32]; // Буфер для ID

const char* AP_SSID = "Kratis-Incubator-01";
const char* AP_PASS = "12345678";

// --- ПІНИ ESP32-C3 SUPER MINI ---
#define I2C_SDA 5
#define I2C_SCL 6
#define BOOT_BUTTON_PIN 9
#define DHTPIN 2      
#define DHTTYPE DHT22 
#define SERVO_PIN 0   // Твій пін для серво

DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Об'єкт сервоприводу
Servo incubatorServo;

KratisNetworkManager* network = nullptr; 

unsigned long lastSensorRead = 0;
unsigned long buttonPressStart = 0;
float temp = 0.0;
float hum = 0.0;
String lastCmd = "Start";

// --- ЗМІННІ ДЛЯ ПЛАВНОГО РУХУ ---
int currentServoAngle = 0; // Поточний реальний кут
int targetServoAngle = 0;  // Кут, до якого треба дійти
const int stepDelay = 60;  // Затримка (швидкість) - чим більше, тим повільніше і плавніше

void onCommandReceived(String cmd, String source) {
    lastCmd = cmd;
    LOGF("[LOG] Command Received: %s via %s\n", cmd.c_str(), source.c_str());
    
    // --- ЛОГІКА СЕРВО ---
    if (cmd.startsWith("SERVO:")) {
        int angle = cmd.substring(6).toInt();
        
        // Захист від дурня (MG996R має межі 0-180)
        if (angle < 0) angle = 0;
        if (angle > 180) angle = 180;
        
        LOGF("[ACTUATOR] New Target Servo Angle: %d\n", angle);
        
        // Встановлюємо нову ціль. 
        // Рух почнеться автоматично в головному циклі loop()
        targetServoAngle = angle;
    }
    // --- ЛОГІКА РЕЛЕ (Світло/Нагрів) ---
    else if (cmd == "LIGHT:ON") {
        // Тут можна додати керування реле
        LOG("[ACTUATOR] Light ON");
    }
    else if (cmd == "LIGHT:OFF") {
        LOG("[ACTUATOR] Light OFF");
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
        // Рядок 1: Статус WiFi
        if (!network->isConnected()) {
            u8g2.drawStr(0, 9, ".NO WIFI.");
        } else if (network->isCloudActive()) {
            u8g2.drawStr(0, 9, "CLOUD");
        } else {
            u8g2.drawStr(0, 9, "LAN");
        }
        
        // Рядок 2: Температура
        u8g2.setCursor(0, 19); 
        u8g2.print("T:"); u8g2.print(temp, 1);
        
        // Рядок 3: Вологість + Кут серви
        u8g2.setCursor(0, 29); 
        u8g2.print("H:"); u8g2.print(hum, 0); 
        u8g2.setCursor(35, 29);
        u8g2.print("S:"); u8g2.print(currentServoAngle);
        // Додаємо індикатор руху
        if(currentServoAngle != targetServoAngle) {
             u8g2.print("->"); 
        }
        
        // Рядок 4: Остання команда (обрізана)
        u8g2.setCursor(0, 39);
        String shortCmd = lastCmd.length() > 10 ? lastCmd.substring(0, 10) : lastCmd;
        u8g2.print(">" + shortCmd);
    }
    u8g2.sendBuffer();
}

void checkHardwareButton() {
    // На C3 кнопка BOOT при натисканні дає LOW
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (buttonPressStart == 0) {
            buttonPressStart = millis();
            LOG("Button Pressed...");
        }
        
        unsigned long duration = millis() - buttonPressStart;

        // Довге натискання > 10с = Скидання
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
            
            // Середнє натискання 3-10с = Режим точки доступу
            if (duration > 3000 && duration < 10000 && network && !network->isApMode()) {
                LOG("Triggering AP MODE (3-10s hold)");
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
    
    generateDeviceId();

    LOG("Init I2C...");
    Wire.begin(I2C_SDA, I2C_SCL);
    
    LOG("Init DHT & OLED...");
    dht.begin();
    u8g2.begin();
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    
    // --- Ініціалізація СЕРВО ---
    LOG("Init Servo MG996R on GPIO 0...");
    
    // Важливо для C3: Виділяємо таймер
    ESP32PWM::allocateTimer(0);
    incubatorServo.setPeriodHertz(50); 
    
    // Використовуємо параметри імпульсу для MG996R
    incubatorServo.attach(SERVO_PIN, 500, 2400);
    
    // Стартова позиція
    incubatorServo.write(0); 
    currentServoAngle = 0;
    targetServoAngle = 0;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "System Init...");
    u8g2.setCursor(0, 30);
    u8g2.print("ID: ");
    // Показуємо тільки хвіст ID
    u8g2.print(String(uniqueDeviceId).substring(10));
    u8g2.sendBuffer();
    delay(2000);

    LOG("Configuring Network Manager...");
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

    // --- ПРІОРИТЕТНА ЛОГІКА РУХУ (BLOCKING MODE) ---
    // Якщо поточний кут не дорівнює цільовому, ми НЕ виконуємо нічого іншого,
    // поки не дійдемо до цілі. Це забезпечує максимальну плавність і точність.
    
    if (currentServoAngle != targetServoAngle) {
        
        // Визначаємо напрямок кроку
        if (currentServoAngle < targetServoAngle) {
            currentServoAngle++;
        } else {
            currentServoAngle--;
        }
        
        // Фізичний рух
        incubatorServo.write(currentServoAngle);
        
        // Оновлюємо екран (щоб бачити прогрес)
        updateDisplay();
        
        // Блокуюча затримка - процесор чекає тільки сервопривід
        delay(stepDelay); 
        
        // RETURN тут критичний: він змушує loop() початися спочатку,
        // пропускаючи network->handle() та зчитування датчиків внизу.
        // Це триватиме доти, доки currentServoAngle не стане == targetServoAngle.
        return; 
    }

    // --- ФОНОВИЙ РЕЖИМ (Тільки коли серво стоїть) ---
    // Цей код виконується тільки тоді, коли мотор досяг цілі.
    
    if(network) network->handle(); // Обробка WiFi та HTTP

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