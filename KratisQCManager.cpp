#include "KratisQCManager.h"

#define DEBUG_QC 1
#if DEBUG_QC
  #define QC_LOG(x) Serial.println(x)
#else
  #define QC_LOG(x)
#endif

// Допоміжна функція для неблокуючої затримки
void safeDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        yield(); // Дозволяємо ESP32 обробляти Wi-Fi стек
        delay(10);
    }
}

KratisQCManager::KratisQCManager(uint8_t dpPin, uint8_t dmPin) {
    _dpPin = dpPin;
    _dmPin = dmPin;
    _isInitialized = false;
}

void KratisQCManager::begin() {
    QC_LOG("[QC_MGR] Initializing Quick Charge Manager...");
    pinMode(_dpPin, INPUT);
    pinMode(_dmPin, INPUT);
}

void KratisQCManager::performHandshake() {
    if (_isInitialized) return;

    QC_LOG("[QC_MGR] Performing QC Handshake sequence...");

    pinMode(_dpPin, INPUT);
    pinMode(_dmPin, INPUT);
    safeDelay(1500); // Неблокуюче очікування

    pinMode(_dpPin, OUTPUT);
    digitalWrite(_dpPin, HIGH); 

    pinMode(_dmPin, OUTPUT);
    digitalWrite(_dmPin, LOW);  

    safeDelay(1500); // Неблокуюче очікування
    
    _isInitialized = true;
    QC_LOG("[QC_MGR] Handshake completed.");
}

void KratisQCManager::forceHandshake() {
    QC_LOG("[QC_MGR] Forcing Handshake...");
    _isInitialized = false; 
    performHandshake();     
    set9V();                
}

void KratisQCManager::set5V() {
    QC_LOG("[QC_MGR] Requesting 5V...");

    pinMode(_dpPin, OUTPUT);
    digitalWrite(_dpPin, LOW);
    pinMode(_dmPin, OUTPUT);
    digitalWrite(_dmPin, LOW);

    QC_LOG("[QC_MGR] 5V target set. QC mode reset.");
    _isInitialized = false; 
}

void KratisQCManager::set9V() {
    QC_LOG("[QC_MGR] Requesting 9V...");
    if (!_isInitialized) performHandshake();

    pinMode(_dpPin, OUTPUT);
    digitalWrite(_dpPin, HIGH);
    pinMode(_dmPin, OUTPUT);
    digitalWrite(_dmPin, HIGH);

    QC_LOG("[QC_MGR] 9V target set (DP=2.7V, DM=1.03V)");
}

void KratisQCManager::set12V() {
    QC_LOG("[QC_MGR] Attempting to set 12V...");
    if (!_isInitialized) performHandshake();
    set9V(); // Обмеження залізом
}