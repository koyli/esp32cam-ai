#include "mqtt_publisher.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

static WiFiClientSecure net;
static PubSubClient     client(net);

bool MqttPublisher::begin() {
    net.setInsecure();
    net.setTimeout(10000);
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setBufferSize(2048);
    return true;
}

static bool ensureConnected() {
    if (client.connected()) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    // Rate-limit reconnect attempts to once every 10 seconds.
    static uint32_t last_attempt = 0;
    if (millis() - last_attempt < 10000) return false;
    last_attempt = millis();
    Serial.println("mqtt: reconnecting");
    return client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
}

bool MqttPublisher::publish(const char* topic, const char* payload) {
    if (!ensureConnected()) return false;
    return client.publish(topic, payload);
}

void MqttPublisher::loop() {
    if (ensureConnected()) client.loop();
}
