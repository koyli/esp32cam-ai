#include "mqtt_publisher.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

static WiFiClient   net;
static PubSubClient client(net);

bool MqttPublisher::begin() {
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setBufferSize(2048);
    return true;
}

static bool ensureConnected() {
    if (client.connected()) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    return client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
}

bool MqttPublisher::publish(const char* topic, const char* payload) {
    if (!ensureConnected()) return false;
    return client.publish(topic, payload);
}

void MqttPublisher::loop() {
    if (ensureConnected()) client.loop();
}
