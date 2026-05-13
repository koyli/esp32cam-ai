#include "claude_client.h"
#include "config.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>

// Reads the JPEG payload of the Nth '00dc' chunk in an MJPEG AVI on the SD
// card and base64-encodes it into out. Returns true on success.
static bool readNthJpeg(const char* avi_path, uint32_t n, String& out) {
    File f = SD_MMC.open(avi_path, FILE_READ);
    if (!f) return false;
    // TODO: scan the 'movi' list for the Nth '00dc' chunk. Stub returns false.
    (void)n; (void)out;
    f.close();
    return false;
}

bool ClaudeClient::summarise(const char* avi_path, uint8_t sample_count, String& out) {
    // TODO: count chunks, pick `sample_count` evenly-spaced indices, base64
    // each via readNthJpeg(), build the messages payload below, POST it.

    WiFiClientSecure tls;
    tls.setInsecure();   // TODO: pin Anthropic's CA root in production
    HTTPClient http;
    String url = String("https://") + CLAUDE_API_HOST + CLAUDE_API_PATH;
    if (!http.begin(tls, url)) return false;

    http.addHeader("content-type", "application/json");
    http.addHeader("x-api-key", CLAUDE_API_KEY);
    http.addHeader("anthropic-version", "2023-06-01");

    JsonDocument req;
    req["model"] = CLAUDE_MODEL;
    req["max_tokens"] = 256;
    JsonArray messages = req["messages"].to<JsonArray>();
    JsonObject user = messages.add<JsonObject>();
    user["role"] = "user";
    JsonArray content = user["content"].to<JsonArray>();
    // TODO: append one image content block per sampled frame, then a final
    // text block: "Describe what is happening across these frames."

    String body; serializeJson(req, body);
    int code = http.POST(body);
    if (code != 200) { http.end(); return false; }

    String resp = http.getString();
    http.end();

    JsonDocument res;
    if (deserializeJson(res, resp)) return false;
    out = res["content"][0]["text"] | "";
    (void)avi_path; (void)sample_count;
    return out.length() > 0;
}
