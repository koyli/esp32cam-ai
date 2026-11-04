#include "trailcam.h"
#include "config.h"

#include <Arduino.h>

#ifndef TRAILCAM_WIFI_PASS

bool trailcamEnabled() { return false; }
int  trailcamFetch() { return 0; }
void trailcamUploadPending() {}

#else

#include <WiFi.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <vector>

#include "upload_client.h"

#ifndef TRAILCAM_BUILD
#error "Trail cameras need Bluetooth, which only fits in IRAM in the trailcam build: pio run -e esp32cam-trailcam"
#endif
#ifndef TRAILCAM_BLE_MATCH
#error "TRAILCAM_WIFI_PASS is set but TRAILCAM_BLE_MATCH isn't; see config.h.in"
#endif
#ifndef TRAILCAM_SD_DIR
#define TRAILCAM_SD_DIR "/trailcam"
#endif
#ifndef TRAILCAM_BLE_SCAN_S
#define TRAILCAM_BLE_SCAN_S 30
#endif
#define TRAILCAM_SENT_DIR TRAILCAM_SD_DIR "/sent"

static const char* const BLE_MATCH[] = {TRAILCAM_BLE_MATCH};

// Every camera serves the same address on its own access point.
static const char*    CAM_HOST = "192.168.8.1";
static const uint16_t CAM_PORT = 8080;

// Wake-up writes found by sniffing the phone app (see koyli/trailcam).
static const char* E7_CHAR = "0000ffb1-0000-1000-8000-00805f9b34fb";
static const char* E7_WAKE = "TCWAKEUP";
static const char* E8_CHAR = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
static const char* E8_WAKE = "AT+WAKEPULSE=10\r\n";

static constexpr int      MAX_ROUNDS  = 20;     // listings per camera (60 files each)
static constexpr uint32_t IO_TIMEOUT  = 15000;  // ms without data before a download is retried

bool trailcamEnabled() { return true; }

// ---------------- Bluetooth wake-up ----------------

static bool matches(BLEAdvertisedDevice& d) {
    String name = d.getName();
    String addr = d.getAddress().toString();
    for (const char* m : BLE_MATCH) {
        if (name.startsWith(m) || addr.startsWith(m)) return true;
    }
    return false;
}

static bool writeWake(BLEClient* c, const char* uuid, const char* payload) {
    auto* services = c->getServices();
    if (!services) return false;
    for (auto& kv : *services) {
        BLERemoteCharacteristic* ch = kv.second->getCharacteristic(uuid);
        if (!ch) continue;
        bool ok = ch->writeValue((uint8_t*)payload, strlen(payload), ch->canWrite());
        Serial.printf("trailcam: wake write to %s %s\n", uuid, ok ? "ok" : "failed");
        return ok;
    }
    return false;
}

// Bluedroid and Wi-Fi together leave about 4 KB of internal RAM, too little to
// run reliably, so Wi-Fi is off while Bluetooth is up and vice versa.
struct Cam {
    BLEAddress addr;
    uint8_t    type;
    String     name;
};

static std::vector<Cam> scanCameras() {
    BLEDevice::init("");
    Serial.printf("trailcam: Bluetooth up, heap %lu\n", (unsigned long)ESP.getFreeHeap());
    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);  // names are in the scan response
    scan->setInterval(100);
    scan->setWindow(99);
    Serial.println("trailcam: scanning Bluetooth");
    BLEScanResults* res = scan->start(TRAILCAM_BLE_SCAN_S, false);

    std::vector<Cam> cams;
    for (int i = 0; res && i < res->getCount(); ++i) {
        BLEAdvertisedDevice d = res->getDevice(i);
        Serial.printf("trailcam: BLE %s \"%s\"\n", d.getAddress().toString().c_str(), d.getName().c_str());
        if (matches(d)) cams.push_back({d.getAddress(), d.getAddressType(), d.getName()});
    }
    scan->clearResults();
    BLEDevice::deinit(false);  // also deletes the scan
    return cams;
}

// The E8 firmware also turns up on some E7s, so try both, likeliest first.
static bool wake(const Cam& cam) {
    BLEDevice::init("");
    BLEClient* c = BLEDevice::createClient();
    bool ok = false;
    if (c->connect(cam.addr, cam.type)) {
        bool e8 = cam.name.endsWith("G_E8");
        ok = e8 ? (writeWake(c, E8_CHAR, E8_WAKE) || writeWake(c, E7_CHAR, E7_WAKE))
                : (writeWake(c, E7_CHAR, E7_WAKE) || writeWake(c, E8_CHAR, E8_WAKE));
        c->disconnect();
        delay(500);  // let the disconnect finish before the stack goes away
    } else {
        Serial.printf("trailcam: BLE connect to %s failed\n", cam.addr.toString().c_str());
    }
    BLEDevice::deinit(false);  // also deletes the client
    return ok;
}

// ---------------- Camera Wi-Fi ----------------

// The camera's SSID is "CAM..." and usually ends with its Bluetooth address, so
// prefer that one in case another camera is also awake.
static bool joinCamWifi(const String& bt_addr) {
    String mac = bt_addr;
    mac.replace(":", "");
    mac.toUpperCase();

    WiFi.disconnect();
    delay(10000);  // the access point takes a few seconds to come up after the wake-up

    String ssid;
    int32_t channel = 0;
    uint8_t bssid[6];
    for (int attempt = 1; attempt <= 4 && ssid.isEmpty(); ++attempt) {
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; ++i) {
            String s = WiFi.SSID(i);
            if (!s.startsWith("CAM")) continue;
            String upper = s;
            upper.toUpperCase();
            if (ssid.isEmpty() || upper.indexOf(mac) >= 0) {
                ssid    = s;
                channel = WiFi.channel(i);
                memcpy(bssid, WiFi.BSSID(i), 6);
            }
            if (upper.indexOf(mac) >= 0) break;
        }
        WiFi.scanDelete();
        if (ssid.isEmpty()) {
            Serial.printf("trailcam: no CAM network yet (scan %d/4)\n", attempt);
            delay(5000);
        }
    }
    if (ssid.isEmpty()) return false;

    Serial.printf("trailcam: joining %s ch%ld\n", ssid.c_str(), (long)channel);
    WiFi.begin(ssid.c_str(), TRAILCAM_WIFI_PASS, channel, bssid);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("trailcam: could not join %s (status %d)\n", ssid.c_str(), WiFi.status());
        return false;
    }
    return true;
}

// ---------------- Camera HTTP API ----------------

// GETs a JSON endpoint. HTTP/1.0 stops the camera using chunked replies, which
// it gets wrong often enough that the Linux client had to retry them.
static bool camJson(const char* path, JsonDocument& doc) {
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(IO_TIMEOUT);
    if (!http.begin(CAM_HOST, CAM_PORT, path)) return false;
    int code = http.GET();
    bool ok = code == 200 && !deserializeJson(doc, http.getStream()) && doc["code"] == 0;
    http.end();
    if (!ok) Serial.printf("trailcam: GET %s failed (HTTP %d)\n", path, code);
    return ok;
}

// Keeps the camera from dropping back to standby mid-transfer.
static void camKeepAwake() {
    JsonDocument doc;
    camJson("/cmd/standby/reset", doc);
}

static bool camGetOk(const char* path) {
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(30000);
    if (!http.begin(CAM_HOST, CAM_PORT, path)) return false;
    int code = http.GET();
    http.end();
    return code == 200;
}

// Saves one file to SD at part_path, retrying from scratch on a bad copy. The
// camera sends chunked downloads, which writeToStream() decodes, so this asks
// for HTTP/1.1 and doesn't read the stream itself.
static bool download(const char* path, const char* part_path, size_t size) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        camKeepAwake();
        HTTPClient http;
        http.setTimeout(IO_TIMEOUT);
        http.setReuse(false);
        if (!http.begin(CAM_HOST, CAM_PORT, path)) return false;
        int code = http.GET();
        if (code != 200) {
            Serial.printf("trailcam: GET %s: HTTP %d\n", path, code);
            http.end();
            continue;
        }
        File f = SD_MMC.open(part_path, FILE_WRITE);
        if (!f) {
            Serial.printf("trailcam: cannot create %s\n", part_path);
            http.end();
            return false;
        }
        uint32_t t0 = millis();
        int got = http.writeToStream(&f);
        uint32_t ms = millis() - t0;
        f.close();
        http.end();

        // got counts bytes received, not stored: a failing card still reports them
        // all, and this copy decides whether the camera's file gets deleted.
        size_t stored = 0;
        f = SD_MMC.open(part_path, FILE_READ);
        if (f) {
            stored = f.size();
            f.close();
        }
        Serial.printf("trailcam: %s: got %d, stored %u of %u in %lu ms\n", path, got, (unsigned)stored,
                      (unsigned)size, (unsigned long)ms);
        if (got > 0 && stored == (size_t)got && (!size || stored == size)) return true;
        SD_MMC.remove(part_path);
    }
    return false;
}

// Size of an already-saved copy (pending or sent), or 0.
static size_t savedSize(const char* name) {
    char p[96];
    for (const char* dir : {TRAILCAM_SD_DIR, TRAILCAM_SENT_DIR}) {
        snprintf(p, sizeof(p), "%s/%s", dir, name);
        File f = SD_MMC.open(p, FILE_READ);
        if (f) {
            size_t n = f.size();
            f.close();
            return n;
        }
    }
    return 0;
}

// Copies everything in one listing; returns the number of files saved.
static int fetchListing(const char* camera) {
    camKeepAwake();
    JsonDocument doc;
    if (!camJson("/list/detail/backward/900000/60", doc)) return 0;
    JsonArray items = doc["data"].as<JsonArray>();
    Serial.printf("trailcam: %s lists %u files\n", camera, (unsigned)items.size());

    int saved = 0;
    for (JsonObject item : items) {
        long        id   = item["id"] | -1L;
        int         type = item["type"] | 0;
        const char* date = item["date"] | "";
        size_t      size = item["size"] | 0;
        if (id < 0) continue;

        const char* ext = type == 1 ? "JPG" : "MP4";
        char stamp[16] = {};
        for (size_t i = 0, j = 0; date[i] && j < sizeof(stamp) - 1; ++i) {
            if (isdigit((unsigned char)date[i])) stamp[j++] = date[i];
        }
        // <camera>_<YYYYMMDDHHMMSS>_<id>.<ext>, the name trailcam-review groups events by.
        char name[64], path[96], part[96], cam_path[48];
        snprintf(name, sizeof(name), "%s_%s_%ld.%s", camera, stamp, id, ext);
        snprintf(path, sizeof(path), "%s/%s", TRAILCAM_SD_DIR, name);
        snprintf(part, sizeof(part), "%s/.%s.part", TRAILCAM_SD_DIR, name);
        snprintf(cam_path, sizeof(cam_path), "/file/%ld/%s", id, ext);

        // A copy from a run whose delete failed: just delete it from the camera.
        bool have = size && savedSize(name) == size;
        if (!have) {
            if (!download(cam_path, part, size)) continue;
            SD_MMC.remove(path);
            if (!SD_MMC.rename(part, path)) continue;
            Serial.printf("trailcam: saved %s (%u bytes)\n", name, (unsigned)size);
            ++saved;
        }
        char del[48];
        snprintf(del, sizeof(del), "/cmd/delete/%ld/%s", id, ext);
        if (!camGetOk(del)) Serial.printf("trailcam: delete of %s on camera failed\n", name);
    }
    return saved;
}

static int fetchCamera() {
    JsonDocument doc;
    camKeepAwake();
    char camera[24] = "CAM";
    if (camJson("/cmd/getSetting", doc)) {
        const char* n = doc["data"]["camera_name"] | "";
        size_t j = 0;
        for (size_t i = 0; n[i] && j < sizeof(camera) - 1; ++i) {
            if (isalnum((unsigned char)n[i])) camera[j++] = n[i];
        }
        if (j) camera[j] = 0;
    }

    int total = 0;
    for (int round = 0; round < MAX_ROUNDS; ++round) {
        int n = fetchListing(camera);
        total += n;
        if (n == 0) break;
    }
    Serial.printf("trailcam: %s: %d files saved\n", camera, total);
    return total;
}

// ---------------- Entry points ----------------

int trailcamFetch() {
    SD_MMC.mkdir(TRAILCAM_SD_DIR);
    SD_MMC.mkdir(TRAILCAM_SENT_DIR);

    WiFi.mode(WIFI_OFF);
    Serial.printf("trailcam: Wi-Fi off, heap %lu\n", (unsigned long)ESP.getFreeHeap());
    std::vector<Cam> cams = scanCameras();
    Serial.printf("trailcam: %u camera(s) found, heap %lu\n", (unsigned)cams.size(),
                  (unsigned long)ESP.getFreeHeap());

    int total = 0;
    for (const Cam& cam : cams) {
        String addr = cam.addr.toString();
        Serial.printf("trailcam: waking %s (%s)\n", cam.name.c_str(), addr.c_str());
        if (!wake(cam)) continue;
        WiFi.mode(WIFI_STA);
        if (joinCamWifi(addr)) total += fetchCamera();
        WiFi.mode(WIFI_OFF);
    }
    return total;
}

void trailcamUploadPending() {
    File dir = SD_MMC.open(TRAILCAM_SD_DIR);
    if (!dir) return;
    // Collect first: renaming entries while iterating the directory isn't safe.
    std::vector<String> names;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory() && f.name()[0] != '.') names.push_back(f.name());
        f.close();
    }
    dir.close();

    int sent = 0;
    for (const String& name : names) {
        String path = String(TRAILCAM_SD_DIR "/") + name;
        if (!uploadFile(path.c_str(), "trailcam")) continue;
        String done = String(TRAILCAM_SENT_DIR "/") + name;
        SD_MMC.remove(done.c_str());
        SD_MMC.rename(path.c_str(), done.c_str());
        ++sent;
    }
    if (!names.empty()) Serial.printf("trailcam: uploaded %d/%u\n", sent, (unsigned)names.size());
}

#endif
