#include "mqtt_publisher.h"
#include "config.h"
#include "ca_certs.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

static WiFiClientSecure net;
static PubSubClient     client(net);

bool MqttPublisher::begin() {
    configureTls(net, MQTT_CA_CERT);
    net.setTimeout(10000);
    net.setHandshakeTimeout(20);  // seconds; the 120 s default would block the main loop
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setBufferSize(2048);
    client.setSocketTimeout(5);  // the connect blocks the main loop, and so motion detection
    return true;
}

static bool ensureConnected() {
    if (client.connected()) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    // After a failure, wait before retrying: each attempt blocks the main loop.
    static uint32_t last_failure = 0;
    static bool     failed       = false;
    if (failed && millis() - last_failure < 60000) return false;
    Serial.println("mqtt: reconnecting");
    uint32_t t0 = millis();
    bool ok = client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    last_failure = millis();
    failed       = !ok;
    Serial.printf("mqtt: %s (state %d) in %lu ms\n", ok ? "connected" : "failed", client.state(),
                  (unsigned long)(last_failure - t0));
    return ok;
}

void MqttPublisher::disconnect() {
    client.disconnect();
    net.stop();
}

bool MqttPublisher::publish(const char* topic, const char* payload) {
    if (!ensureConnected()) return false;
    return client.publish(topic, payload);
}

void MqttPublisher::loop() {
    if (ensureConnected()) client.loop();
}
