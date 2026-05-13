#pragma once

// ---------------- Wi-Fi ----------------
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASSWORD   "YOUR_PASSWORD"

// ---------------- MQTT -----------------
#define MQTT_HOST       "broker.example.com"
#define MQTT_PORT       1883
#define MQTT_USER       ""
#define MQTT_PASS       ""
#define MQTT_TOPIC      "esp32cam-ai/events"
#define MQTT_CLIENT_ID  "esp32cam-ai"

// ---------------- Claude API -----------
#define CLAUDE_API_HOST   "api.anthropic.com"
#define CLAUDE_API_PATH   "/v1/messages"
#define CLAUDE_API_KEY    "sk-ant-REPLACE_ME"
#define CLAUDE_MODEL      "claude-opus-4-7"

// ---------------- Capture --------------
// Ring buffer holds the most recent PRE_FRAMES; on a motion trigger we also
// record POST_FRAMES afterward, so each AVI contains PRE_FRAMES + POST_FRAMES.
#define PRE_FRAMES        30
#define POST_FRAMES       60

// Frames sampled (evenly) from each AVI for the Claude API call.
#define CLAUDE_SAMPLE_FRAMES  8

// Motion detection: pixel-difference threshold as a percentage of the frame.
// A frame whose downsampled luma differs from the previous by more than this
// fraction of pixels triggers a recording.
#define MOTION_THRESHOLD_PCT  5

// SD card path for recordings and summaries.
#define SD_RECORDINGS_DIR     "/recordings"
