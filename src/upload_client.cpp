#include "upload_client.h"
#include "config.h"

#include <WiFiClientSecure.h>
#include <SD_MMC.h>

bool uploadFile(const char* sd_path) {
    File f = SD_MMC.open(sd_path, FILE_READ);
    if (!f) {
        Serial.printf("upload: cannot open %s\n", sd_path);
        return false;
    }
    const size_t file_size = f.size();

    const char* filename = strrchr(sd_path, '/');
    filename = filename ? filename + 1 : sd_path;

    Serial.printf("upload: %s (%u bytes)\n", filename, (unsigned)file_size);

    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(30000);
    if (!tls.connect(UPLOAD_HOST, UPLOAD_PORT)) {
        Serial.println("upload: TLS connect failed");
        f.close();
        return false;
    }

    tls.printf("POST /upload HTTP/1.0\r\n");
    tls.printf("Host: %s\r\n", UPLOAD_HOST);
    tls.printf("Content-Type: application/octet-stream\r\n");
    tls.printf("Content-Length: %u\r\n", (unsigned)file_size);
    tls.printf("X-Upload-Token: %s\r\n", UPLOAD_TOKEN);
    tls.printf("X-Filename: %s\r\n\r\n", filename);

    auto* buf = (uint8_t*)malloc(2048);
    if (!buf) { tls.stop(); f.close(); return false; }

    size_t sent = 0;
    uint32_t deadline = millis() + 90000;
    while (f.available() && millis() < deadline) {
        size_t n = f.read(buf, 2048);
        if (!n) break;
        size_t w = tls.write(buf, n);
        sent += w;
        if (w != n) {
            Serial.printf("upload: write stalled at %u/%u\n", (unsigned)sent, (unsigned)file_size);
            break;
        }
    }
    free(buf);
    f.close();
    Serial.printf("upload: sent %u/%u bytes\n", (unsigned)sent, (unsigned)file_size);

    uint32_t t0 = millis();
    while (!tls.available() && millis() - t0 < 15000) delay(10);
    String status = tls.readStringUntil('\n');
    tls.stop();
    status.trim();
    Serial.printf("upload: response: %s\n", status.c_str());

    int code = status.length() >= 12 ? status.substring(9, 12).toInt() : 0;
    return code == 200 || code == 201;
}
