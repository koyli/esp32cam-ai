#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_camera.h>

#include "config.h"
#include "camera_pins.h"
#include "ring_buffer.h"
#include "motion_detect.h"
#include "avi_writer.h"
#include "claude_client.h"
#include "mqtt_publisher.h"

static JpegRingBuffer ring(PRE_FRAMES);
static MotionDetector motion(MOTION_THRESHOLD_PCT);
static MqttPublisher  mqtt;
static ClaudeClient   claude;

static bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
    c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href  = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn  = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_VGA;
    c.jpeg_quality = 12;
    c.fb_count     = 2;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    c.grab_mode    = CAMERA_GRAB_LATEST;
    return esp_camera_init(&c) == ESP_OK;
}

static bool initWifi() {
    WiFi.mode(WIFI_STA);

    int n = WiFi.scanNetworks();
    if (n <= 0) return false;

    // Find the known network with the best RSSI.
    const WifiCred* best = nullptr;
    int32_t best_rssi = INT32_MIN;
    static constexpr size_t CRED_COUNT = sizeof(WIFI_CREDS) / sizeof(WIFI_CREDS[0]);

    for (int i = 0; i < n; ++i) {
        String found = WiFi.SSID(i);
        for (size_t j = 0; j < CRED_COUNT; ++j) {
            if (found == WIFI_CREDS[j].ssid && WiFi.RSSI(i) > best_rssi) {
                best_rssi = WiFi.RSSI(i);
                best = &WIFI_CREDS[j];
            }
        }
    }
    WiFi.scanDelete();

    if (!best) return false;

    Serial.printf("wifi: connecting to %s (%d dBm)\n", best->ssid, best_rssi);
    WiFi.begin(best->ssid, best->pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi: connected, IP %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    return false;
}

static void recordClip(const camera_fb_t* trigger_fb);

void setup() {
    Serial.begin(115200);
    Serial.println("\nesp32cam-ai boot");

    if (!SD_MMC.begin("/sdcard", true)) {       // 1-bit mode, GPIO4 free
        Serial.println("SD init failed");
    }
    if (!SD_MMC.exists(SD_RECORDINGS_DIR)) {
        SD_MMC.mkdir(SD_RECORDINGS_DIR);
    }
    if (!initCamera()) {
        Serial.println("camera init failed");
        while (true) delay(1000);
    }
    if (!initWifi()) {
        Serial.println("wifi connect failed (continuing)");
    }
    mqtt.begin();

    Serial.println("boot: recording startup clip");
    recordClip(nullptr);
}

void loop() {
    mqtt.loop();

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { delay(10); return; }

    static uint32_t last_heartbeat = 0;
    static uint32_t frame_count = 0;
    static int      peak_changed = 0;

    frame_count++;
    int changed = motion.update(fb);
    if (changed > peak_changed) peak_changed = changed;

    uint32_t now = millis();
    if (now - last_heartbeat >= 10000) {
        Serial.printf("status: %lu frames, peak changed %d/%d cells, heap %lu\n",
                      frame_count, peak_changed, motion.thresholdCells(),
                      (unsigned long)ESP.getFreeHeap());
        peak_changed = 0;
        last_heartbeat = now;
    }

    ring.push(fb);

    if (motion.triggered(changed)) {
        recordClip(fb);
    }

    esp_camera_fb_return(fb);
}

// Captures POST_FRAMES additional frames, writes the AVI containing the ring
// buffer + new frames, asks Claude for a summary, saves it, publishes via MQTT.
static void recordClip(const camera_fb_t* /*trigger_fb*/) {
    char avi_path[64];
    snprintf(avi_path, sizeof(avi_path), "%s/clip_%lu.avi",
             SD_RECORDINGS_DIR, (unsigned long)millis());
    Serial.printf("motion: recording %s\n", avi_path);

    AviWriter w;
    if (!w.begin(avi_path, 640, 480, 10)) return;

    for (size_t i = 0; i < ring.size(); ++i) {
        w.writeFrame(ring.data(i), ring.len(i));
    }
    for (int i = 0; i < POST_FRAMES; ++i) {
        camera_fb_t* f = esp_camera_fb_get();
        if (!f) continue;
        w.writeFrame(f->buf, f->len);
        esp_camera_fb_return(f);
    }
    w.end();

    String summary, plates;
    if (!claude.summarise(avi_path, CLAUDE_SAMPLE_FRAMES, summary, plates)) {
        summary = "{\"description\":\"(ai summary unavailable)\",\"has_vehicle\":false,\"plates\":[]}"; // file injected below
    }

    char txt_path[80];
    snprintf(txt_path, sizeof(txt_path), "%s.txt", avi_path);
    File t = SD_MMC.open(txt_path, FILE_WRITE);
    if (t) { t.print(summary); t.close(); }

    // Inject file path into the JSON object before publishing.
    String payload = String("{\"file\":\"") + avi_path + "\"," + summary.substring(1);

    Serial.printf("summary: %s\n", payload.c_str());
    if (plates.length()) Serial.printf("plates: %s\n", plates.c_str());

    Serial.printf("motion: publishing to %s\n", MQTT_TOPIC);
    bool ok = mqtt.publish(MQTT_TOPIC, payload.c_str());
    Serial.printf("motion: publish %s\n", ok ? "ok" : "failed");

    if (plates.length()) {
        Serial.printf("motion: publishing plates to %s\n", MQTT_PLATES_TOPIC);
        mqtt.publish(MQTT_PLATES_TOPIC, plates.c_str());
    }
}
