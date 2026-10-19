#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD_MMC.h>
#include <esp_camera.h>
#include <Preferences.h>
#include <time.h>

#include "config.h"
#include "camera_pins.h"
#include "ring_buffer.h"
#include "motion_detect.h"
#include "avi_writer.h"
#include "claude_client.h"
#include "mqtt_publisher.h"
#include "upload_client.h"

static JpegRingBuffer ring(PRE_FRAMES);
static MotionDetector motion(MOTION_THRESHOLD_PCT);
static MqttPublisher  mqtt;
static ClaudeClient   claude;

static char     device_id[5];  // 4 hex chars derived from the factory MAC
static uint32_t boot_count;    // persisted in NVS; disambiguates clips when NTP is unavailable

// CRC-16/CCITT of the factory MAC, so filenames from different devices don't
// collide when they land in the same upload directory.
static void initDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 6; ++i) {
        crc ^= (uint16_t)((mac >> (8 * i)) & 0xFF) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    snprintf(device_id, sizeof(device_id), "%04X", crc);

    Preferences prefs;
    prefs.begin("esp32cam-ai", false);
    boot_count = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", boot_count);
    prefs.end();
}

static bool timeValid() {
    return time(nullptr) > 1700000000;  // anything before Nov 2023 means SNTP hasn't synced
}

// Starts SNTP (it keeps re-syncing in the background) and waits briefly for a first sync.
static void initTime() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    uint32_t start = millis();
    while (WiFi.status() == WL_CONNECTED && !timeValid() && millis() - start < 5000) delay(100);
    Serial.printf("time: %s\n", timeValid() ? "synced" : "not synced (using boot counter)");
}

// <dir>/<id>_YYYYMMDD_HHMMSS.avi (UTC), or <dir>/<id>_b<boot>_<millis>.avi before
// SNTP has synced. A numeric suffix is added if the name already exists.
static void makeClipPath(char* out, size_t n) {
    char stem[48];
    time_t now = time(nullptr);
    if (timeValid()) {
        struct tm tm;
        gmtime_r(&now, &tm);
        char ts[20];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
        snprintf(stem, sizeof(stem), "%s/%s_%s", SD_RECORDINGS_DIR, device_id, ts);
    } else {
        snprintf(stem, sizeof(stem), "%s/%s_b%lu_%lu", SD_RECORDINGS_DIR, device_id,
                 (unsigned long)boot_count, (unsigned long)millis());
    }
    snprintf(out, n, "%s.avi", stem);
    for (int i = 1; SD_MMC.exists(out) && i < 100; ++i) {
        snprintf(out, n, "%s_%d.avi", stem, i);
    }
}

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
    WiFi.persistent(false);       // don't save credentials to flash
    WiFi.disconnect(true, true);  // disconnect and erase any stored AP
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    esp_wifi_set_ps(WIFI_PS_NONE); // disable power saving — improves association reliability

    int n = WiFi.scanNetworks();
    if (n <= 0) return false;

    // Find the known network with the best RSSI, saving BSSID+channel for direct connect.
    const WifiCred* best = nullptr;
    int32_t best_rssi    = INT32_MIN;
    uint8_t best_bssid[6] = {};
    uint8_t best_channel  = 0;
    static constexpr size_t CRED_COUNT = sizeof(WIFI_CREDS) / sizeof(WIFI_CREDS[0]);

    for (int i = 0; i < n; ++i) {
        String found = WiFi.SSID(i);
        for (size_t j = 0; j < CRED_COUNT; ++j) {
            if (found == WIFI_CREDS[j].ssid && WiFi.RSSI(i) > best_rssi) {
                best_rssi    = WiFi.RSSI(i);
                best_channel = (uint8_t)WiFi.channel(i);
                memcpy(best_bssid, WiFi.BSSID(i), 6);
                best = &WIFI_CREDS[j];
            }
        }
    }
    WiFi.scanDelete();

    if (!best) return false;

    // Give the radio a moment to exit scan mode.
    delay(200);

    Serial.printf("wifi: connecting to %s ch%d (%d dBm)\n", best->ssid, best_channel, best_rssi);

    // Use esp_wifi directly with BSSID+channel so the driver associates without
    // a second internal scan — that second scan is what was producing NO_AP_FOUND.
    wifi_config_t cfg = {};
    strlcpy((char*)cfg.sta.ssid,     best->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, best->pass, sizeof(cfg.sta.password));
    cfg.sta.channel   = best_channel;
    cfg.sta.bssid_set = 1;
    memcpy(cfg.sta.bssid, best_bssid, 6);

    for (int attempt = 1; attempt <= 3; ++attempt) {
        // Full stack reset before each attempt flushes the PMKSA cache and clears
        // any stale 802.11 auth state that causes AUTH_EXPIRE on the AP side.
        esp_wifi_stop();
        delay(300);
        esp_wifi_start();
        delay(300);

        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_connect();
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("wifi: connected, IP %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        Serial.printf("wifi: attempt %d failed (status %d), retrying\n", attempt, WiFi.status());
        esp_wifi_disconnect();
        delay(1000);
    }
    return false;
}

static void recordClip();

static bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("wifi: disconnected, attempting reconnect");
    return initWifi();
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nesp32cam-ai boot");
    initDeviceId();
    Serial.printf("device id %s, boot %lu\n", device_id, (unsigned long)boot_count);

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
    initTime();
    mqtt.begin();

    Serial.println("boot: recording startup clip");
    recordClip();
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
                      (unsigned long)frame_count, peak_changed, motion.thresholdCells(),
                      (unsigned long)ESP.getFreeHeap());
        peak_changed = 0;
        last_heartbeat = now;
    }

    ring.push(fb);
    esp_camera_fb_return(fb);

    if (motion.triggered(changed)) {
        recordClip();
    }
}

// Captures POST_FRAMES additional frames, writes the AVI containing the ring
// buffer + new frames, asks Claude for a summary, saves it, publishes via MQTT.
static void recordClip() {
    char avi_path[64];
    makeClipPath(avi_path, sizeof(avi_path));
    Serial.printf("motion: recording %s\n", avi_path);

    AviWriter w;
    if (!w.begin(avi_path, 640, 480)) return;

    // Pre-trigger frames arrive at the main loop's rate (which includes motion
    // decoding). Space the post-trigger frames at the same interval so the whole
    // clip plays back at a consistent speed.
    const size_t pre = ring.size();
    uint32_t interval_ms = 100;
    if (pre >= 2) interval_ms = (ring.time(pre - 1) - ring.time(0)) / (pre - 1);

    uint32_t frames  = 0;
    uint32_t first_t = 0, last_t = 0;
    for (size_t i = 0; i < pre; ++i) {
        w.writeFrame(ring.data(i), ring.len(i));
        if (!frames++) first_t = ring.time(i);
        last_t = ring.time(i);
    }
    for (int i = 0; i < POST_FRAMES; ++i) {
        if (frames) {
            while (millis() - last_t < interval_ms) delay(1);
        }
        camera_fb_t* f = esp_camera_fb_get();
        if (!f) continue;
        uint32_t t = millis();
        w.writeFrame(f->buf, f->len);
        esp_camera_fb_return(f);
        if (!frames++) first_t = t;
        last_t = t;
    }
    uint32_t us_per_frame = frames >= 2 ? (uint32_t)((uint64_t)(last_t - first_t) * 1000 / (frames - 1))
                                        : interval_ms * 1000;
    Serial.printf("motion: %lu frames, %.1f fps\n", (unsigned long)frames,
                  us_per_frame ? 1e6f / us_per_frame : 0.0f);
    w.end(us_per_frame);

    ensureWifi();

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

    uploadFile(avi_path);
    uploadFile(txt_path);

    // Recording, AI calls and uploads take a long time. Drop the stale frames so the
    // next comparison isn't against a scene from a minute ago (which would re-trigger)
    // and the next clip's lead-in frames are actually from just before its trigger.
    motion.reset();
    ring.clear();
}
