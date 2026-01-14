#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>

// --- КОНФІГУРАЦІЯ ---
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* SERVER_BASE_URL = "https://kratis-p2p-server.onrender.com"; 
const char* DEVICE_ID = "esp32_device_01";

// I2C Піни (перевір свої!)
#define I2C_SDA 5
#define I2C_SCL 6

// --- ТАЙМІНГИ ---
const unsigned long LOCAL_TIMEOUT = 10000; 
const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long CLOUD_POLL_INTERVAL = 3000;

// --- ОБЛАДНАННЯ ---
#define DHTPIN 2      
#define DHTTYPE DHT22 
DHT dht(DHTPIN, DHTTYPE);
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// --- ГЛОБАЛЬНІ ---
WiFiMulti wifiMulti;
WebServer server(80);

unsigned long lastPollTime = 0;
unsigned long lastSensorRead = 0;
unsigned long lastLocalActivity = 0;

float temp = 0.0;
float hum = 0.0;
String lastCmd = "Wait WiFi";
bool cloudModeActive = true; 
bool isServerStarted = false; // Прапорець, чи запущено веб-сервер

// --- ФУНКЦІЇ ---

void updateDisplay() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    // Рядок 1: Статус WiFi
    if (WiFi.status() != WL_CONNECTED) {
        u8g2.drawStr(0, 9, ".NO WIFI.");
    } else if (cloudModeActive) {
        u8g2.drawStr(0, 9, "CLOUD MODE");
    } else {
        u8g2.drawStr(0, 9, "LAN MODE");
    }

    // Рядок 2-3: Дані
    u8g2.drawStr(0, 19, "T:"); u8g2.setCursor(20, 19); u8g2.print(temp, 1);
    u8g2.drawStr(0, 29, "H:"); u8g2.setCursor(20, 29); u8g2.print(hum, 1);
    
    // Рядок 4: Команда
    u8g2.setCursor(0, 39);
    u8g2.print(">" + lastCmd);
    u8g2.sendBuffer();
}

void executeCommand(String cmd, String source) {
    lastCmd = cmd;
    Serial.println("[EXEC] " + cmd + " via " + source);
    if (cmd.startsWith("SERVO:")) {
        int angle = cmd.substring(6).toInt();
        Serial.printf("Servo: %d\n", angle);
    }
    updateDisplay();
}

// ЛОКАЛЬНИЙ ОБРОБНИК (LAN)
void handleLocalRequest() {
    lastLocalActivity = millis();
    
    if (cloudModeActive) {
        cloudModeActive = false;
        updateDisplay();
    }

    if (server.hasArg("val")) {
        executeCommand(server.arg("val"), "LAN");
    }

    StaticJsonDocument<200> doc;
    doc["status"] = "ok";
    doc["temp"] = temp;
    doc["hum"] = hum;
    
    String response;
    serializeJson(doc, response);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", response);
}

// ХМАРНИЙ ОБРОБНИК (CLOUD)
void pollCloudServer() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.setTimeout(3000); 
    String url = String(SERVER_BASE_URL) + "/api/sync";
    
    StaticJsonDocument<256> doc;
    doc["id"] = DEVICE_ID;
    doc["local_ip"] = WiFi.localIP().toString();
    
    JsonObject data = doc.createNestedObject("data");
    data["temp"] = temp;
    data["hum"] = hum;
    
    String jsonBody;
    serializeJson(doc, jsonBody);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    int code = http.POST(jsonBody);

    if (code == 200) {
        String resp = http.getString();
        StaticJsonDocument<200> respDoc;
        DeserializationError error = deserializeJson(respDoc, resp);
        if (!error && respDoc.containsKey("cmd")) {
            executeCommand(String(respDoc["cmd"]), "CLOUD");
        }
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    
    // 1. I2C (Критично для екрану)
    Wire.begin(I2C_SDA, I2C_SCL);
    
    // 2. Залізо
    dht.begin();
    u8g2.begin();
    
    // Одразу малюємо, не чекаючи WiFi
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "System Init...");
    u8g2.sendBuffer();

    // 3. Налаштування WiFi (але не чекаємо підключення!)
    wifiMulti.addAP(WIFI_SSID, WIFI_PASS);
    
    // Налаштовуємо ендпоінти сервера (але `server.begin()` викличемо, коли буде мережа)
    server.on("/cmd", HTTP_GET, handleLocalRequest);
    server.on("/status", HTTP_GET, handleLocalRequest);
    server.onNotFound([]() { server.send(204); });

    // Початковий час
    lastLocalActivity = millis() - LOCAL_TIMEOUT - 1000;
}

void loop() {
    // 1. Фонове підтримання WiFi
    // Ця функція не блокує цикл, якщо WiFi немає, вона просто намагається підключитися
    wifiMulti.run();

    // 2. Логіка при наявності/відсутності мережі
    if (WiFi.status() == WL_CONNECTED) {
        // Якщо підключились вперше - стартуємо сервер
        if (!isServerStarted) {
            server.begin();
            isServerStarted = true;
            lastCmd = "WiFi OK";
            Serial.println("WiFi Connected. Server Started.");
            // Перший стук в хмару для реєстрації IP
            pollCloudServer();
        }
        
        // Обробка локальних клієнтів
        server.handleClient();
    } else {
        // Якщо WiFi відпав
        isServerStarted = false;
    }

    // 3. Читання сенсорів (ПРАЦЮЄ ЗАВЖДИ, незалежно від WiFi)
    if (millis() - lastSensorRead > SENSOR_INTERVAL) {
        lastSensorRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        
        if (!isnan(t)) temp = t;
        if (!isnan(h)) hum = h;
        
        // Оновлюємо екран при кожній зміні даних
        updateDisplay();
    }

    // 4. Логіка Хмари (Тільки якщо є WiFi)
    if (WiFi.status() == WL_CONNECTED) {
        if (millis() - lastLocalActivity > LOCAL_TIMEOUT) {
            // Локально тихо
            if (!cloudModeActive) {
                cloudModeActive = true;
                updateDisplay();
            }
            // Стукаємо на сервер
            if (millis() - lastPollTime > CLOUD_POLL_INTERVAL) {
                lastPollTime = millis();
                pollCloudServer();
            }
        }
    }
}