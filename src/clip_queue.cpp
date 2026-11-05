#include "clip_queue.h"
#include "config.h"
#include "trailcam.h"
#include "upload_client.h"

#include <SD_MMC.h>
#include <vector>

#ifndef SD_MIN_FREE_MB
#define SD_MIN_FREE_MB 200
#endif

static constexpr uint32_t GAP_MS      = 5000;  // between files, so motion detection gets a turn
static constexpr uint32_t RETRY_MIN_S = 300;   // first wait after a failure, doubling each time
static constexpr uint32_t RETRY_MAX_S = 3600;
static constexpr size_t   PRUNE_BATCH = 32;    // most files removed from a folder per call

static bool     maybe_pending = true;  // the last boot may have left files waiting
static String   next_file;
static uint32_t wait_from     = 0;
static uint32_t wait_ms       = 0;
static uint32_t retry_s       = 0;

void clipQueueNotify() { maybe_pending = true; }

// Names sort by time for clips stamped from a synced clock.
static String oldestPending() {
    String oldest;
    File dir = SD_MMC.open(SD_PENDING_DIR);
    if (!dir) return oldest;
    bool is_dir;
    for (String name = dir.getNextFileName(&is_dir); name.length(); name = dir.getNextFileName(&is_dir)) {
        if (!is_dir && (!oldest.length() || name < oldest)) oldest = name;
    }
    dir.close();
    return oldest;
}

bool clipQueueDue() {
    if (!maybe_pending || millis() - wait_from < wait_ms) return false;
    next_file = oldestPending();
    maybe_pending = next_file.length() > 0;
    return maybe_pending;
}

static void waitAfterFailure() {
    retry_s = retry_s ? min(retry_s * 2, RETRY_MAX_S) : RETRY_MIN_S;
    wait_from = millis();
    wait_ms   = retry_s * 1000;
    Serial.printf("queue: retrying in %lu s\n", (unsigned long)retry_s);
}

void clipQueueSend() {
    const String& path = next_file;
    UploadResult r = uploadFile(path.c_str());
    if (r == UploadResult::Failed) {
        waitAfterFailure();
        return;
    }
    if (r == UploadResult::Rejected) Serial.printf("queue: server refused %s\n", path.c_str());

    // A refused file moves too: it would fail again and hold up the rest.
    String done = String(SD_RECORDINGS_DIR) + path.substring(path.lastIndexOf('/'));
    SD_MMC.remove(done.c_str());
    if (!SD_MMC.rename(path.c_str(), done.c_str())) {
        Serial.printf("queue: cannot move %s\n", path.c_str());
        waitAfterFailure();
        return;
    }
    retry_s   = 0;
    wait_from = millis();
    wait_ms   = GAP_MS;
}

static uint64_t sdFree() { return SD_MMC.totalBytes() - SD_MMC.usedBytes(); }

// Everything here is already on the server, so directory order is good enough:
// it's roughly oldest first, and needs no clock or per-file lookups.
static int pruneDir(const char* path, uint64_t want_free) {
    File dir = SD_MMC.open(path);
    if (!dir) return 0;
    // Collect first: changing entries while iterating the directory isn't safe.
    std::vector<String> names;
    bool is_dir;
    for (String name = dir.getNextFileName(&is_dir); name.length() && names.size() < PRUNE_BATCH;
         name = dir.getNextFileName(&is_dir)) {
        if (!is_dir) names.push_back(name);
    }
    dir.close();

    int removed = 0;
    for (const String& name : names) {
        if (sdFree() >= want_free) break;
        if (SD_MMC.remove(name.c_str())) ++removed;
    }
    return removed;
}

void sdPrune() {
    const uint64_t want_free = (uint64_t)SD_MIN_FREE_MB * 1024 * 1024;
    if (sdFree() >= want_free) return;

    int removed = pruneDir(SD_RECORDINGS_DIR, want_free);
    if (const char* sent = trailcamSentDir()) removed += pruneDir(sent, want_free);
    Serial.printf("sd: removed %d uploaded files, %llu MB free\n", removed,
                  (unsigned long long)(sdFree() / (1024 * 1024)));
}
