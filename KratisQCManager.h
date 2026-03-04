#ifndef KRATIS_QC_MANAGER_H
#define KRATIS_QC_MANAGER_H

#include <Arduino.h>

class KratisQCManager {
public:
    KratisQCManager(uint8_t dpPin, uint8_t dmPin);

    void begin();

    void set5V();
    void set9V();
    void set12V();

   
    void forceHandshake();

private:
    uint8_t _dpPin;
    uint8_t _dmPin;
    bool _isInitialized;

    void performHandshake();
};

#endif
