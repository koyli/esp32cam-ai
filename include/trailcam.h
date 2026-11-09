#pragma once

// Optional trail-camera collector (GardePro E7/E8 style cameras). Enabled by
// defining TRAILCAM_WIFI_PASS in config.h; otherwise every function here is a
// no-op. See the "Trail cameras" section of config.h.in.

bool trailcamEnabled();

// Wakes each matching camera over Bluetooth LE, joins its "CAM..." Wi-Fi access
// point and copies new photos and videos to SD in batches of up to
// TRAILCAM_BATCH_MB, uploading each batch (via upstream(), which reconnects to
// the upstream network) before fetching the next. A file is deleted from the
// camera only once the server has it. Stops at the first failed upload or after
// TRAILCAM_MAX_MIN, leaving the rest on the camera. Call while connected
// upstream; returns connected again if possible.
void trailcamCollect(bool (*upstream)());

// Folder of files that have reached the server, or nullptr when disabled.
const char* trailcamSentDir();
