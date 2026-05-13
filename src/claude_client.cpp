#include "claude_client.h"
#include "config.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>

// Matches the exact layout produced by AviWriter: 12 (RIFF) + 200 (LIST hdrl) + 12 (LIST movi).
static constexpr uint32_t MOVI_DATA_OFFSET = 224;

static constexpr size_t MAX_FRAME_SLOTS  = 96;
static constexpr size_t JPEG_BUF_SIZE    = 60 * 1024;   // headroom above 40 KB VGA JPEG
static constexpr size_t BODY_CAP         = 512 * 1024;

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

bool ClaudeClient::summarise(const char* avi_path, uint8_t sample_count, String& out) {
    auto* infos   = (ChunkInfo*)heap_caps_malloc(MAX_FRAME_SLOTS * sizeof(ChunkInfo), MALLOC_CAP_SPIRAM);
    auto* jpeg    = (uint8_t*) heap_caps_malloc(JPEG_BUF_SIZE,                         MALLOC_CAP_SPIRAM);
    auto* body    = (char*)    heap_caps_malloc(BODY_CAP,                               MALLOC_CAP_SPIRAM);

    if (!infos || !jpeg || !body) {
        heap_caps_free(infos); heap_caps_free(jpeg); heap_caps_free(body);
        return false;
    }

    File f = SD_MMC.open(avi_path, FILE_READ);
    if (!f) {
        heap_caps_free(infos); heap_caps_free(jpeg); heap_caps_free(body);
        return false;
    }

    size_t total = scanFrames(f, infos, MAX_FRAME_SLOTS);
    if (total == 0) {
        f.close();
        heap_caps_free(infos); heap_caps_free(jpeg); heap_caps_free(body);
        return false;
    }

    if (sample_count > (uint8_t)total) sample_count = (uint8_t)total;

    size_t pos = (size_t)snprintf(body, BODY_CAP,
        "{\"model\":\"%s\",\"max_tokens\":256,"
        "\"messages\":[{\"role\":\"user\",\"content\":[",
        CLAUDE_MODEL);

    bool first = true;
    for (uint8_t i = 0; i < sample_count; ++i) {
        const ChunkInfo& ci = infos[frameIndex(i, sample_count, total)];
        if (ci.size == 0 || ci.size > JPEG_BUF_SIZE) continue;
        if (!f.seek(ci.data_offset)) continue;
        if ((size_t)f.read(jpeg, ci.size) != ci.size) continue;

        // Base64-encode the JPEG. Output length = ceil(ci.size/3)*4.
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
        "\"Describe concisely what is happening across these frames.\"}]"
        "}]}",
        first ? "" : ",");

    WiFiClientSecure tls;
    tls.setInsecure();   // TODO: pin Anthropic's CA root in production
    HTTPClient http;
    http.setTimeout(30000);
    String url = String("https://") + CLAUDE_API_HOST + CLAUDE_API_PATH;
    if (!http.begin(tls, url)) { heap_caps_free(body); return false; }

    http.addHeader("content-type", "application/json");
    http.addHeader("x-api-key", CLAUDE_API_KEY);
    http.addHeader("anthropic-version", "2023-06-01");

    int code = http.POST((uint8_t*)body, pos);
    heap_caps_free(body);

    if (code != 200) { http.end(); return false; }

    String resp = http.getString();
    http.end();

    JsonDocument res;
    if (deserializeJson(res, resp)) return false;
    out = res["content"][0]["text"] | "";
    return out.length() > 0;
}
