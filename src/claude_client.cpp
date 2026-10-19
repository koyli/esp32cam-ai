#include "claude_client.h"
#include "config.h"
#include "ca_certs.h"

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

// Matches the exact layout produced by AviWriter: 12 (RIFF) + 200 (LIST hdrl) + 12 (LIST movi).
static constexpr uint32_t MOVI_DATA_OFFSET = 224;

static constexpr size_t MAX_FRAME_SLOTS = 96;
static constexpr size_t JPEG_BUF_SIZE   = 60 * 1024;
static constexpr size_t BODY_CAP        = 512 * 1024;
// Space kept free while adding image frames, so the prompt and closing JSON always fit.
static constexpr size_t BODY_TAIL_RESERVE = 2048;

#ifndef CLAUDE_MODEL_PARAMS
#define CLAUDE_MODEL_PARAMS ""
#endif
#ifndef CLAUDE_MODEL_BETA
#define CLAUDE_MODEL_BETA ""
#endif

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

// Appends formatted text at buf+pos without writing past cap. On truncation,
// leaves pos unchanged (buf stays NUL-terminated there) and returns false.
static bool appendf(char* buf, size_t cap, size_t& pos, const char* fmt, ...) {
    if (pos >= cap) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - pos) {
        buf[pos] = '\0';
        return false;
    }
    pos += (size_t)n;
    return true;
}

// Shared HTTP/TLS send+receive logic — body is in PSRAM, chunked into DRAM for DMA.
static int tlsPost(WiFiClientSecure& tls, const char* body, size_t body_len, String& resp) {
    static const size_t CHUNK = 2048;
    auto* chunk = (uint8_t*)malloc(CHUNK);
    if (!chunk) { tls.stop(); return 0; }
    size_t sent = 0;
    uint32_t write_deadline = millis() + 30000;
    while (sent < body_len) {
        if (millis() > write_deadline) {
            Serial.println("ai: write timeout");
            break;
        }
        size_t n = body_len - sent; if (n > CHUNK) n = CHUNK;
        memcpy(chunk, body + sent, n);
        size_t w = tls.write(chunk, n);
        if (!w) { Serial.printf("ai: write stalled at %u/%u\n", (unsigned)sent, (unsigned)body_len); break; }
        sent += w;
    }
    free(chunk);
    Serial.printf("ai: sent %u/%u bytes\n", (unsigned)sent, (unsigned)body_len);

    // Wait for first byte with timeout.
    uint32_t t0 = millis();
    while (!tls.available() && millis() - t0 < 30000) delay(10);
    String status = tls.readStringUntil('\n'); status.trim();
    Serial.println("ai: " + status);
    int code = status.length() >= 12 ? status.substring(9, 12).toInt() : 0;

    // Consume headers.
    while (tls.connected() || tls.available()) {
        String line = tls.readStringUntil('\n'); line.trim();
        if (!line.length()) break;
    }
    // Drain body until 500ms of silence — more reliable than connected()/available()
    // flags on ESP32, which can both drop before the last bytes are buffered.
    uint32_t idle = millis();
    while (millis() - idle < 500) {
        if (tls.available()) {
            resp += (char)tls.read();
            idle = millis();
        } else {
            delay(1);
        }
    }
    tls.stop();
    return code;
}

// --- Backend-specific POST ---

// Claude — always compiled (primary or fallback depending on USE_GEMINI).
static int claudePost(const char* model, const char* body, size_t body_len, String& resp) {
    WiFiClientSecure tls;
    configureTls(tls, ROOT_CA_BUNDLE);
    tls.setTimeout(10000);
    if (!tls.connect(CLAUDE_API_HOST, 443)) {
        Serial.println("ai: TLS connect failed");
        return 0;
    }
    tls.printf("POST %s HTTP/1.0\r\n", CLAUDE_API_PATH);
    tls.printf("Host: %s\r\n", CLAUDE_API_HOST);
    tls.printf("Content-Type: application/json\r\n");
    tls.printf("x-api-key: %s\r\n", CLAUDE_API_KEY);
    tls.printf("anthropic-version: 2023-06-01\r\n");
#ifndef USE_GEMINI
    if (CLAUDE_MODEL_BETA[0] && strcmp(model, CLAUDE_MODEL) == 0) {
        tls.printf("anthropic-beta: %s\r\n", CLAUDE_MODEL_BETA);
    }
#else
    (void)model;
#endif
    tls.printf("Content-Length: %u\r\n\r\n", (unsigned)body_len);
    return tlsPost(tls, body, body_len, resp);
}

#ifdef USE_GEMINI
static int geminiPost(const char* model, const char* body, size_t body_len, String& resp) {
    WiFiClientSecure tls;
    configureTls(tls, ROOT_CA_BUNDLE);
    tls.setTimeout(10000);
    if (!tls.connect("generativelanguage.googleapis.com", 443)) {
        Serial.println("ai: TLS connect failed");
        return 0;
    }
    // Key goes in a header rather than the URL, so it doesn't end up in logs.
    tls.printf("POST /v1beta/models/%s:generateContent HTTP/1.0\r\n", model);
    tls.printf("Host: generativelanguage.googleapis.com\r\n");
    tls.printf("Content-Type: application/json\r\n");
    tls.printf("x-goog-api-key: %s\r\n", GEMINI_API_KEY);
    tls.printf("Content-Length: %u\r\n\r\n", (unsigned)body_len);
    return tlsPost(tls, body, body_len, resp);
}
#endif

static int aiPost(const char* model, const char* body, size_t body_len, String& resp) {
#ifdef USE_GEMINI
    return geminiPost(model, body, body_len, resp);
#else
    return claudePost(model, body, body_len, resp);
#endif
}

// Retry once on any non-200 response or empty body.
static int aiPostWithRetry(const char* model, const char* body, size_t body_len, String& resp) {
    int code = aiPost(model, body, body_len, resp);
    if (code != 200 || !resp.length()) {
        Serial.println("ai: retrying...");
        resp = "";
        code = aiPost(model, body, body_len, resp);
    }
    return code;
}

#ifdef USE_GEMINI
static constexpr bool PRIMARY_IS_GEMINI = true;
static const char*    PRIMARY_MODEL     = GEMINI_MODEL;
static const char*    PRIMARY_PARAMS    = "";
static const char*    PLATES_MODEL      = GEMINI_PLATES_MODEL;
#else
static constexpr bool PRIMARY_IS_GEMINI = false;
static const char*    PRIMARY_MODEL     = CLAUDE_MODEL;
static const char*    PRIMARY_PARAMS    = CLAUDE_MODEL_PARAMS;
static const char*    PLATES_MODEL      = CLAUDE_PLATES_MODEL;
#endif

static const char* SCENE_PROMPT =
    "Analyse these frames and respond with ONLY valid JSON in this exact format: "
    "{\\\"description\\\": \\\"brief scene description\\\", "
    "\\\"has_vehicle\\\": true, "
    "\\\"plates\\\": [\\\"plate1\\\", \\\"plate2\\\"]}. "
    "Set has_vehicle to true only if a vehicle (car, van, truck, bus, motorbike etc) "
    "is clearly present. "
    "The plates array should contain any UK or Irish vehicle registration plates "
    "you can read clearly. If no plates are visible use an empty array.";

static const char* PLATE_PROMPT =
    "List all UK and Irish vehicle registration plates visible in this image. "
    "Respond ONLY with valid JSON: {\\\"plates\\\": [\\\"plate1\\\"]}. "
    "Use an empty array if none are visible.";

// PSRAM allocation freed when it goes out of scope.
struct PsramBuf {
    void* p;
    explicit PsramBuf(size_t n) : p(heap_caps_malloc(n, MALLOC_CAP_SPIRAM)) {}
    ~PsramBuf() { heap_caps_free(p); }
    PsramBuf(const PsramBuf&) = delete;
    PsramBuf& operator=(const PsramBuf&) = delete;
    template <typename T> T* as() const { return static_cast<T*>(p); }
};

// --- Request building (Gemini generateContent or Claude Messages) ---

// Opens the request JSON, up to the first content part. extra is inserted
// verbatim into a Claude request (each field followed by a comma).
static bool beginRequest(char* buf, size_t& pos, bool gemini, const char* model,
                         int max_tokens, const char* extra) {
    pos = 0;
    if (gemini) return appendf(buf, BODY_CAP, pos, "{\"contents\":[{\"parts\":[");
    return appendf(buf, BODY_CAP, pos,
        "{\"model\":\"%s\",\"max_tokens\":%d,%s"
        "\"messages\":[{\"role\":\"user\",\"content\":[",
        model, max_tokens, extra);
}

// Appends a base64 JPEG content part, encoding straight into buf. On failure
// pos is restored and false returned.
static bool appendImage(char* buf, size_t cap, size_t& pos, bool gemini,
                        const uint8_t* jpeg, size_t len, bool comma) {
    const size_t start = pos;
    bool ok = appendf(buf, cap, pos,
        gemini ? "%s{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\""
               : "%s{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/jpeg\",\"data\":\"",
        comma ? "," : "");
    if (ok) {
        size_t n = 0;
        ok = mbedtls_base64_encode((unsigned char*)buf + pos, cap - pos, &n, jpeg, len) == 0;
        if (ok) pos += n;
    }
    ok = ok && appendf(buf, cap, pos, "\"}}");
    if (!ok) {
        pos = start;
        buf[pos] = '\0';
    }
    return ok;
}

// Appends the text prompt and closes the request JSON.
static bool endRequest(char* buf, size_t& pos, bool gemini, const char* prompt,
                       int max_tokens, bool comma) {
    if (gemini) {
        return appendf(buf, BODY_CAP, pos,
            "%s{\"text\":\"%s\"}]}],\"generationConfig\":{\"maxOutputTokens\":%d}}",
            comma ? "," : "", prompt, max_tokens);
    }
    return appendf(buf, BODY_CAP, pos,
        "%s{\"type\":\"text\",\"text\":\"%s\"}]}]}",
        comma ? "," : "", prompt);
}

static bool buildSingleImageRequest(char* buf, size_t& pos, bool gemini, const char* model,
                                    int max_tokens, const char* extra,
                                    const uint8_t* jpeg, size_t len, const char* prompt) {
    return beginRequest(buf, pos, gemini, model, max_tokens, extra)
        && appendImage(buf, BODY_CAP, pos, gemini, jpeg, len, false)
        && endRequest(buf, pos, gemini, prompt, max_tokens, true);
}

// --- Response parsing ---

// Returns the first text block of a Claude response. Blocks are matched by type,
// since a response can start with a (possibly empty) thinking block. A refusal
// yields an empty string so the caller treats it as a failure.
static String extractClaudeText(JsonDocument& doc) {
    if (doc["stop_reason"] == "refusal") {
        Serial.printf("ai: Claude refused (%s)\n", (const char*)(doc["stop_details"]["category"] | "unknown"));
        return "";
    }
    for (JsonVariant block : doc["content"].as<JsonArray>()) {
        if (block["type"] == "text") return block["text"] | "";
    }
    return "";
}

// Parses an API response whose text is itself a JSON object (optionally in a
// code fence). On success text holds that JSON and inner its parsed form.
static bool parseReply(const String& resp, bool gemini, String& text, JsonDocument& inner) {
    JsonDocument res;
    if (deserializeJson(res, resp)) {
        Serial.println("ai: JSON parse failed");
        return false;
    }
    text = gemini ? String(res["candidates"][0]["content"]["parts"][0]["text"] | "")
                  : extractClaudeText(res);
    if (!text.length()) return false;
    stripCodeFence(text);
    if (deserializeJson(inner, text)) {
        Serial.println("ai: inner JSON parse failed");
        return false;
    }
    return true;
}

static String joinPlates(JsonDocument& inner) {
    String plates;
    for (JsonVariant p : inner["plates"].as<JsonArray>()) {
        if (plates.length()) plates += ", ";
        plates += p.as<String>();
    }
    return plates;
}

bool ClaudeClient::summarise(const char* avi_path, uint8_t sample_count, String& out, String& plates) {
    PsramBuf body(BODY_CAP);
    PsramBuf mid_frame(JPEG_BUF_SIZE);  // middle sample, reused for the single-image calls
    if (!body.p || !mid_frame.p) {
        Serial.println("ai: PSRAM alloc failed");
        return false;
    }
    char* req = body.as<char>();
    size_t pos = 0;
    size_t mid_len = 0;

    // --- Scene-analysis request: sample_count frames spread across the clip ---
    {
        PsramBuf infos(MAX_FRAME_SLOTS * sizeof(ChunkInfo));
        PsramBuf jpeg(JPEG_BUF_SIZE);
        if (!infos.p || !jpeg.p) {
            Serial.println("ai: PSRAM alloc failed");
            return false;
        }

        File f = SD_MMC.open(avi_path, FILE_READ);
        if (!f) {
            Serial.printf("ai: cannot open %s\n", avi_path);
            return false;
        }

        size_t total = scanFrames(f, infos.as<ChunkInfo>(), MAX_FRAME_SLOTS);
        Serial.printf("ai: %u frames found in AVI\n", (unsigned)total);
        if (total == 0) {
            f.close();
            return false;
        }

        if (sample_count > total) sample_count = (uint8_t)total;
        uint8_t mid_sample = sample_count / 2;

        beginRequest(req, pos, PRIMARY_IS_GEMINI, PRIMARY_MODEL, 512, PRIMARY_PARAMS);
        bool first = true;
        for (uint8_t i = 0; i < sample_count; ++i) {
            const ChunkInfo& ci = infos.as<ChunkInfo>()[frameIndex(i, sample_count, total)];
            if (ci.size == 0 || ci.size > JPEG_BUF_SIZE) continue;
            if (!f.seek(ci.data_offset)) continue;
            if ((size_t)f.read(jpeg.as<uint8_t>(), ci.size) != ci.size) continue;

            if (i == mid_sample) {
                memcpy(mid_frame.p, jpeg.p, ci.size);
                mid_len = ci.size;
            }
            if (!appendImage(req, BODY_CAP - BODY_TAIL_RESERVE, pos, PRIMARY_IS_GEMINI,
                             jpeg.as<uint8_t>(), ci.size, !first)) {
                Serial.printf("ai: request body full, sending %u of %u frames\n", (unsigned)i, (unsigned)sample_count);
                break;
            }
            first = false;
        }
        f.close();

        if (!endRequest(req, pos, PRIMARY_IS_GEMINI, SCENE_PROMPT, 512, !first)) {
            Serial.println("ai: request body overflow");
            return false;
        }
    }

    String resp;
    String text;
    JsonDocument scene;
    int code = aiPostWithRetry(PRIMARY_MODEL, req, pos, resp);

#ifdef USE_GEMINI
    // Fallback: if Gemini failed, one Claude Haiku call with the middle frame
    // (no separate plate-reading call).
    if (code != 200 || !resp.length()) {
        Serial.println("ai: Gemini failed, falling back to Claude Haiku");
        resp = "";
        if (!mid_len || !buildSingleImageRequest(req, pos, false, CLAUDE_HAIKU_MODEL, 512, "",
                                                 mid_frame.as<uint8_t>(), mid_len, SCENE_PROMPT)) {
            return false;
        }
        code = claudePost(CLAUDE_HAIKU_MODEL, req, pos, resp);
        if (code != 200 || !resp.length()) {
            Serial.printf("ai: Claude Haiku fallback failed (%d)\n", code);
            return false;
        }
        if (!parseReply(resp, false, text, scene)) return false;
        out = text;
        plates = joinPlates(scene);
        return out.length() > 0;
    }
#endif

    if (code != 200) {
        Serial.println("ai: " + resp.substring(0, 300));
        return false;
    }
    if (!parseReply(resp, PRIMARY_IS_GEMINI, text, scene)) return false;
    out = text;
    plates = joinPlates(scene);

    // --- Plate-reading call on the middle frame when a vehicle is present ---
    if ((scene["has_vehicle"] | false) && mid_len) {
        Serial.println("ai: vehicle detected, running VGA plate-reading call");
        String plate_resp, plate_text;
        JsonDocument plate_doc;
        if (buildSingleImageRequest(req, pos, PRIMARY_IS_GEMINI, PLATES_MODEL, 128, "",
                                    mid_frame.as<uint8_t>(), mid_len, PLATE_PROMPT)
            && aiPostWithRetry(PLATES_MODEL, req, pos, plate_resp) == 200
            && parseReply(plate_resp, PRIMARY_IS_GEMINI, plate_text, plate_doc)) {
            String vga_plates = joinPlates(plate_doc);
            if (vga_plates.length()) {
                plates = vga_plates;
                Serial.printf("ai: plates (VGA): %s\n", plates.c_str());
            } else {
                Serial.println("ai: plate call returned no results, keeping scene analysis plates");
            }
        }
    }

    return out.length() > 0;
}
