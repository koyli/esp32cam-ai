# esp32cam-ai

Motion-triggered ESP32-CAM (AI-Thinker) recorder: keeps a ring buffer of
frames, writes an AVI to SD when motion is detected, asks Gemini/Claude to
summarise it, publishes the summary over MQTT and uploads the clip.

## Setup

Copy `include/config.h.in` to `include/config.h`, fill in your credentials,
then `pio run -t upload`.

The build uses the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform (Arduino-ESP32 3.x on ESP-IDF 5.x), which needs PlatformIO running on
Python 3.10–3.14. If your `pio` uses a newer Python, run it through uv:
`uvx --python 3.14 --from platformio pio run -t upload`.

## Wi-Fi extender (optional)

Define `EXTENDER_SSID` and `EXTENDER_PASS` in `config.h` and the camera also
runs an access point whose clients are NATed onto the upstream network, so it
works as a small range extender while it watches for motion.

- The AP runs on the upstream network's channel (a single-radio limitation);
  clients briefly drop when the camera reconnects upstream.
- Its subnet, `EXTENDER_IP`/24 (default 192.168.71.0/24), must not overlap
  the upstream network. Clients get the upstream DNS server via DHCP.
- Throughput is modest: a few Mbit/s, shared with the camera's own uploads
  and dropping while it records or does TLS. Fine for an IoT device or a
  phone's light browsing, not for streaming.
- The serial status line shows the number of connected AP clients.
- Range is that of the board's antenna: tens of metres indoors with the PCB
  antenna, noticeably more with an external antenna on the IPEX connector
  (move the 0 Ω resistor next to it to select it).

## Trail cameras (optional)

Define `TRAILCAM_WIFI_PASS` and `TRAILCAM_BLE_MATCH` in `config.h` (see
`config.h.in`) and build the `esp32cam-trailcam` environment:
`pio run -e esp32cam-trailcam -t upload`. At each hour in `TRAILCAM_HOURS` (UTC,
default 00/06/12/18) and once at boot, the camera:

1. scans Bluetooth for trail cameras whose name or address starts with a
   `TRAILCAM_BLE_MATCH` entry and wakes each one (the E7 and E8 wake-ups from
   [koyli/trailcam](https://github.com/koyli/trailcam));
2. joins the camera's `CAM...` Wi-Fi and copies new photos and videos to
   `/trailcam` on the SD card as `<camera>_<YYYYMMDDHHMMSS>_<id>.JPG|MP4`,
   deleting each from the trail camera once its size checks out;
3. reconnects upstream and uploads them to the server, which saves them in
   `~/capture/trailcam` for trailcam-review. Uploaded files move to
   `/trailcam/sent`; failed ones are retried on the next run.

Motion capture pauses while this runs, and the upstream link (and any extender
clients) drops. The separate environment exists because Bluetooth doesn't fit
in IRAM next to Wi-Fi and the camera with the prebuilt libraries; it rebuilds
them to let IRAM spill into SRAM1 and with a BLE-only Bluetooth controller, and
its first build takes a few minutes. On the server, `~/capture/trailcam` must be writable by the
upload_server container's user (1001).
