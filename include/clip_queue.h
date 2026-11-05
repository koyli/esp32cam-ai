#pragma once

// Clips wait in SD_PENDING_DIR until the server confirms them, then move up to
// SD_RECORDINGS_DIR. The connection may be down or very slow, and an upload
// stops motion detection, so sending is spread out: one file at a time, with a
// growing wait after a failure.
#define SD_PENDING_DIR SD_RECORDINGS_DIR "/pending"

// Call after adding files to SD_PENDING_DIR.
void clipQueueNotify();

// True when a file is waiting and no wait is in progress.
bool clipQueueDue();

// Sends the oldest waiting file, after clipQueueDue() returns true. Needs the upstream connection and the TLS
// memory, so the caller must have closed MQTT.
void clipQueueSend();

// Deletes uploaded files until SD_MIN_FREE_MB is free. Files still waiting to
// upload are never deleted.
void sdPrune();
