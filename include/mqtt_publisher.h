#pragma once

#include <Arduino.h>

class MqttPublisher {
public:
    bool begin();          // connects (or reconnects) using config.h values
    bool publish(const char* topic, const char* payload);
    void loop();           // call from main loop to keep the connection alive
};
