#include "claude_client.h"
#include "config.h"

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>

// Matches the exact layout produced by AviWriter: 12 (RIFF) + 200 (LIST hdrl) + 12 (LIST movi).
static constexpr uint32_t MOVI_DATA_OFFSET = 224;

static constexpr size_t MAX_FRAME_SLOTS = 96;
static constexpr size_t JPEG_BUF_SIZE   = 60 * 1024;
static constexpr size_t BODY_CAP        = 512 * 1024;

struct ChunkInfo { uint32_t data_offset; uint32_t size; };

// Single-pass scan of '00dc' chunks starting at MOVI_DATA_OFFSET.
static size_t scanFrames(File& f, ChunkInfo* out, size_t max) {
    size_t count = 0;
    if (!f.seek(MOVI_DATA_OFFSET)) return 0;

    uint8_t hdr[8];
    while (count < max && f.read(hdr, 8) == 8) {
        uint32_t sz = (uint32_t)hdr[4]
                    | ((uint32_t)hdr[5] << 8)
                    | ((uint32_t)hdr[6] << 16)
                    | ((uint32_t)hdr[7] << 24);

        if (hdr[0]=='0' && hdr[1]=='0' && hdr[2]=='d' && hdr[3]=='c') {
            out[count++] = { (uint32_t)f.position(), sz };
        }
        if (!f.seek(f.position() + sz + (sz & 1))) break;
    }
    return count;
}

static size_t frameIndex(uint8_t i, uint8_t sample_count, size_t total) {
    if (sample_count <= 1 || total <= 1) return total / 2;
    return (size_t)i * (total - 1) / (sample_count - 1);
}


static void stripCodeFence(String& s) {
    s.trim();
    if (s.startsWith("```")) {
        int nl = s.indexOf('\n');
        if (nl >= 0) s = s.substring(nl + 1);
        s.trim();
        if (s.endsWith("```")) s = s.substring(0, s.length() - 3);
        s.trim();
    }
}

static bool vehicleInScene(const String& desc) {
    String s = desc; s.toLowerCase();
    const char* kw[] = {
        "car", "vehicle", "truck", "van", "bus", "lorry",
        "motorbike", "motorcycle", "suv", "4x4", nullptr
    };
    for (int i = 0; kw[i]; ++i)
        if (s.indexOf(kw[i]) >= 0) return true;
    return false;
}

// POST body (PSRAM, body_len bytes) to the Claude messages API.
// Returns the HTTP status code; appends the response body to resp.
static int claudePost(const char* body, size_t body_len, String& resp) {
    WiFiClientSecure tls;
    tls.setInsecure();

    Serial.printf("claude: connecting to %s\n", CLAUDE_API_HOST);
    if (!tls.connect(CLAUDE_API_HOST, 443)) {
        Serial.println("claude: TLS connect failed");
        return 0;
    }

    tls.printf("POST %s HTTP/1.0\r\n", CLAUDE_API_PATH);
    tls.printf("Host: %s\r\n", CLAUDE_API_HOST);
    tls.printf("Content-Type: application/json\r\n");
    tls.printf("x-api-key: %s\r\n", CLAUDE_API_KEY);
    tls.printf("anthropic-version: 2023-06-01\r\n");
    tls.printf("Content-Length: %u\r\n\r\n", (unsigned)body_len);

    // Send from PSRAM in small DRAM chunks (ESP32 AES DMA cannot access PSRAM).
    static const size_t CHUNK = 2048;
    auto* chunk = (uint8_t*)malloc(CHUNK);
    if (!chunk) { tls.stop(); return 0; }
    size_t sent = 0;
    while (sent < body_len) {
        size_t n = body_len - sent; if (n > CHUNK) n = CHUNK;
        memcpy(chunk, body + sent, n);
        size_t w = tls.write(chunk, n);
        if (!w) { Serial.printf("claude: write stalled at %u/%u\n", (unsigned)sent, (unsigned)body_len); break; }
        sent += w;
    }
    free(chunk);
    Serial.printf("claude: sent %u/%u bytes\n", (unsigned)sent, (unsigned)body_len);

    uint32_t t0 = millis();
    while (!tls.available() && millis() - t0 < 30000) delay(10);
    String status = tls.readStringUntil('\n'); status.trim();
    Serial.println("claude: " + status);
    int code = status.length() >= 12 ? status.substring(9, 12).toInt() : 0;

    while (tls.connected() || tls.available()) {
        String line = tls.readStringUntil('\n'); line.trim();
        if (!line.length()) break;
    }

    t0 = millis();
    while ((tls.connected() || tls.available()) && millis() - t0 < 30000) {
        while (tls.available()) resp += (char)tls.read();
        delay(1);
    }
    tls.stop();
    return code;
}

bool ClaudeClient::summarise(const char* avi_path, uint8_t sample_count, String& out, String& plates) {
    auto* infos     = (ChunkInfo*)heap_caps_malloc(MAX_FRAME_SLOTS * sizeof(ChunkInfo), MALLOC_CAP_SPIRAM);
    auto* jpeg      = (uint8_t*) heap_caps_malloc(JPEG_BUF_SIZE,                        MALLOC_CAP_SPIRAM);
    auto* body      = (char*)    heap_caps_malloc(BODY_CAP,                              MALLOC_CAP_SPIRAM);
    auto* vga_plate = (uint8_t*) heap_caps_malloc(JPEG_BUF_SIZE,                        MALLOC_CAP_SPIRAM);

    if (!infos || !jpeg || !body || !vga_plate) {
        Serial.println("claude: PSRAM alloc failed");
        heap_caps_free(infos); heap_caps_free(jpeg);
        heap_caps_free(body);  heap_caps_free(vga_plate);
        return false;
    }
    size_t vga_plate_len = 0;

    File f = SD_MMC.open(avi_path, FILE_READ);
    if (!f) {
        Serial.printf("claude: cannot open %s\n", avi_path);
        heap_caps_free(infos); heap_caps_free(jpeg);
        heap_caps_free(body);  heap_caps_free(vga_plate);
        return false;
    }

    size_t total = scanFrames(f, infos, MAX_FRAME_SLOTS);
    Serial.printf("claude: %u frames found in AVI\n", (unsigned)total);
    if (total == 0) {
        f.close();
        heap_caps_free(infos); heap_caps_free(jpeg);
        heap_caps_free(body);  heap_caps_free(vga_plate);
        return false;
    }

    if (sample_count > (uint8_t)total) sample_count = (uint8_t)total;
    uint8_t mid_sample = sample_count / 2;

    // --- Build scene-analysis request (VGA frames → Sonnet) ---
    size_t pos = (size_t)snprintf(body, BODY_CAP,
        "{\"model\":\"%s\",\"max_tokens\":512,"
        "\"messages\":[{\"role\":\"user\",\"content\":[",
        CLAUDE_MODEL);

    bool first = true;
    for (uint8_t i = 0; i < sample_count; ++i) {
        const ChunkInfo& ci = infos[frameIndex(i, sample_count, total)];
        if (ci.size == 0 || ci.size > JPEG_BUF_SIZE) continue;
        if (!f.seek(ci.data_offset)) continue;
        if ((size_t)f.read(jpeg, ci.size) != ci.size) continue;

        // Save the middle sampled frame for the plate-reading call.
        if (i == mid_sample) {
            memcpy(vga_plate, jpeg, ci.size);
            vga_plate_len = ci.size;
        }

        size_t b64_cap = ((ci.size + 2) / 3) * 4 + 1;
        auto* b64 = (char*)heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
        if (!b64) continue;
        size_t b64_len = 0;
        if (mbedtls_base64_encode((unsigned char*)b64, b64_cap, &b64_len, jpeg, ci.size) != 0) {
            heap_caps_free(b64); continue;
        }
        b64[b64_len] = '\0';

        if (!first && pos < BODY_CAP - 1) body[pos++] = ',';
        first = false;
        pos += (size_t)snprintf(body + pos, BODY_CAP - pos,
            "{\"type\":\"image\",\"source\":{"
            "\"type\":\"base64\","
            "\"media_type\":\"image/jpeg\","
            "\"data\":\"%s\"}}", b64);
        heap_caps_free(b64);
    }
    f.close();
    heap_caps_free(infos);
    heap_caps_free(jpeg);

    pos += (size_t)snprintf(body + pos, BODY_CAP - pos,
        "%s{\"type\":\"text\",\"text\":"
        "\"Analyse these frames and respond with ONLY valid JSON in this exact format: "
        "{\\\"description\\\": \\\"brief scene description\\\", "
        "\\\"plates\\\": [\\\"plate1\\\", \\\"plate2\\\"]}. "
        "The plates array should contain any UK or Irish vehicle registration plates "
        "you can read clearly. If no plates are visible use an empty array.\"}]"
        "}]}",
        first ? "" : ",");

    String resp;
    int code = claudePost(body, pos, resp);
    if (code != 200) {
        Serial.println("claude: " + resp.substring(0, 300));
        heap_caps_free(body); heap_caps_free(vga_plate);
        return false;
    }

    JsonDocument res;
    if (deserializeJson(res, resp)) {
        Serial.println("claude: JSON parse failed");
        heap_caps_free(body); heap_caps_free(vga_plate);
        return false;
    }
    String text = res["content"][0]["text"] | "";
    if (!text.length()) {
        heap_caps_free(body); heap_caps_free(vga_plate);
        return false;
    }

    stripCodeFence(text);
    JsonDocument inner;
    if (deserializeJson(inner, text)) {
        out = text; plates = "";
        heap_caps_free(body); heap_caps_free(vga_plate);
        return true;
    }
    out = text;  // full clean JSON for MQTT
    String scene_desc = inner["description"] | String("");
    plates = "";
    JsonArray arr = inner["plates"].as<JsonArray>();
    for (JsonVariant p : arr) {
        if (plates.length()) plates += ", ";
        plates += p.as<String>();
    }

    // --- Plate-reading call when a vehicle is present (VGA frame → Haiku) ---
    if (vehicleInScene(scene_desc) && vga_plate_len > 0) {
        Serial.println("claude: vehicle detected, running VGA plate-reading call");

        size_t b64_cap = ((vga_plate_len + 2) / 3) * 4 + 1;
        auto* b64 = (char*)heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
        if (b64) {
            size_t b64_len = 0;
            if (mbedtls_base64_encode((unsigned char*)b64, b64_cap, &b64_len, vga_plate, vga_plate_len) == 0) {
                b64[b64_len] = '\0';
                pos = (size_t)snprintf(body, BODY_CAP,
                    "{\"model\":\"%s\",\"max_tokens\":128,"
                    "\"messages\":[{\"role\":\"user\",\"content\":["
                    "{\"type\":\"image\",\"source\":{"
                    "\"type\":\"base64\","
                    "\"media_type\":\"image/jpeg\","
                    "\"data\":\"%s\"}},"
                    "{\"type\":\"text\",\"text\":"
                    "\"List all UK and Irish vehicle registration plates visible in this image. "
                    "Respond ONLY with valid JSON: {\\\"plates\\\": [\\\"plate1\\\"]}. "
                    "Use an empty array if none are visible.\"}]}]}",
                    CLAUDE_PLATES_MODEL, b64);

                String plate_resp;
                if (claudePost(body, pos, plate_resp) == 200) {
                    JsonDocument pres;
                    if (!deserializeJson(pres, plate_resp)) {
                        String ptext = pres["content"][0]["text"] | "";
                        stripCodeFence(ptext);
                        JsonDocument pinner;
                        if (!deserializeJson(pinner, ptext)) {
                            String vga_plates;
                            JsonArray parr = pinner["plates"].as<JsonArray>();
                            for (JsonVariant p : parr) {
                                if (vga_plates.length()) vga_plates += ", ";
                                vga_plates += p.as<String>();
                            }
                            // Haiku result on the dedicated mid-frame supersedes Sonnet estimate.
                            plates = vga_plates;
                            if (plates.length())
                                Serial.printf("claude: plates (VGA): %s\n", plates.c_str());
                        }
                    }
                }
            }
            heap_caps_free(b64);
        }
    }

    heap_caps_free(body);
    heap_caps_free(vga_plate);
    return out.length() > 0;
}
