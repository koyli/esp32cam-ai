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
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setBufferSize(2048);
    client.setSocketTimeout(5);  // the connect blocks the main loop, and so motion detection
    return true;
}

static bool ensureConnected() {
    if (client.connected()) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    // Rate-limit reconnect attempts, measured from the end of the last one
    // since it can take several seconds.
    static uint32_t last_attempt = 0;
    static bool     attempted    = false;
    if (attempted && millis() - last_attempt < 60000) return false;
    Serial.println("mqtt: reconnecting");
    uint32_t t0 = millis();
    bool ok = client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    last_attempt = millis();
    attempted    = true;
    Serial.printf("mqtt: %s (state %d) in %lu ms\n", ok ? "connected" : "failed", client.state(),
                  (unsigned long)(last_attempt - t0));
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
