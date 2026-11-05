#pragma once

// Optional trail-camera collector (GardePro E7/E8 style cameras). Enabled by
// defining TRAILCAM_WIFI_PASS in config.h; otherwise every function here is a
// no-op. See the "Trail cameras" section of config.h.in.

bool trailcamEnabled();

// Wakes each matching camera over Bluetooth LE, joins its "CAM..." Wi-Fi access
// point and copies new photos and videos to SD, deleting each from the camera
// once its copy is verified. Leaves the station disconnected from the upstream
// network: the caller must reconnect before uploading. Returns files saved.
int trailcamFetch();

// Uploads trail-camera files on SD that haven't been uploaded yet, moving each
// into the "sent" folder once the server has it. Needs the upstream connection.
void trailcamUploadPending();

// Folder of files that have reached the server, or nullptr when disabled.
const char* trailcamSentDir();
