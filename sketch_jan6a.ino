#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include "KratisNetworkManager.h" // Використовуємо наш новий клас

// --- КОНФІГУРАЦІЯ ---
const char* SERVER_URL = "https://kratis-p2p-server.onrender.com"; 
const char* DEVICE_ID = "esp32_device_01";

// Піни (ВАЖЛИВО: I2C піни 5 і 6, як ви просили)
#define I2C_SDA 5
#define I2C_SCL 6
#define BOOT_BUTTON_PIN 9
#define DHTPIN 2      
#define DHTTYPE DHT22 

// --- ОБ'ЄКТИ ---
DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Створюємо менеджера мережі
KratisNetworkManager network(SERVER_URL, DEVICE_ID);

// --- ЗМІННІ ---
unsigned long lastSensorRead = 0;
unsigned long buttonPressStart = 0;
float temp = 0.0;
float hum = 0.0;
String lastCmd = "Start";

// --- БІЗНЕС ЛОГІКА ---

// Ця функція викличеться автоматично, коли прийде команда з мережі
void onCommandReceived(String cmd, String source) {
    lastCmd = cmd;
    Serial.println("[MAIN] Execute: " + cmd + " (via " + source + ")");
    
    // Тут керуємо реле, сервоприводом тощо
    if (cmd.startsWith("SERVO:")) {
        int angle = cmd.substring(6).toInt();
        Serial.printf("Moving Servo to %d\n", angle);
    }
}

void updateDisplay() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    if (network.isApMode()) {
        u8g2.drawStr(0, 9, "SETUP MODE");
        u8g2.drawStr(0, 19, "WiFi: Smart-Inc");
        u8g2.drawStr(0, 29, "192.168.4.1");
    } else {
        // Рядок статусу
        if (!network.isConnected()) {
            u8g2.drawStr(0, 9, ".NO WIFI.");
        } else if (network.isCloudActive()) {
            u8g2.drawStr(0, 9, "CLOUD MODE");
        } else {
            u8g2.drawStr(0, 9, "LAN MODE");
        }
        
        // Дані
        u8g2.drawStr(0, 19, "T:"); u8g2.setCursor(20, 19); u8g2.print(temp, 1);
        u8g2.drawStr(0, 29, "H:"); u8g2.setCursor(20, 29); u8g2.print(hum, 1);
        
        // Остання команда
        u8g2.setCursor(0, 39);
        u8g2.print(">" + lastCmd);
    }
    u8g2.sendBuffer();
}

void checkHardwareButton() {
    // Логіка кнопки BOOT (для входу в AP або скидання)
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (buttonPressStart == 0) buttonPressStart = millis();
        unsigned long duration = millis() - buttonPressStart;

        if (duration > 10000) { // 10 сек -> Скидання
            network.factoryReset();
        } else if (duration > 3000 && !network.isApMode()) { // 3 сек -> AP Mode
            // Чекаємо відпускання
            while(digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);
            network.startApMode();
        }
    } else {
        buttonPressStart = 0;
    }
}

void setup() {
    Serial.begin(115200);
    
    // Ініціалізація заліза
    Wire.begin(I2C_SDA, I2C_SCL);
    dht.begin();
    u8g2.begin();
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "System Init...");
    u8g2.sendBuffer();

    // Налаштування мережі
    // Передаємо функцію, яка буде виконувати команди
    network.setCommandCallback(onCommandReceived);
    network.begin();
}

void loop() {
    // 1. Опитуємо кнопку
    checkHardwareButton();

    // 2. Даємо час мережевому менеджеру
    network.handle();

    // 3. Читаємо сенсори
    if (millis() - lastSensorRead > 2000) {
        lastSensorRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        
        if (!isnan(t)) temp = t;
        if (!isnan(h)) hum = h;

        // ВАЖЛИВО: Передаємо дані в менеджер, щоб він міг їх відправити
        network.updateSensorData(temp, hum);
        
        updateDisplay();
    }
}