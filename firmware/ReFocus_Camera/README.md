# ReFocus Camera — ESP32-P4 Firmware

ESP-IDF firmware for the **Waveshare ESP32-P4-WiFi6** board that integrates
camera capture, SD-card storage and a Wi-Fi access-point web server compatible
with the **ReFocus PWA**.

---

## Feature Summary

| Feature | Detail |
|---------|--------|
| **Trigger** | Physical BOOT button (GPIO 0) |
| **On press** | Capture JPEG → save to SD card as `img_NNN.jpg` |
| **Idle sleep** | Light-sleep after **10 seconds** of inactivity |
| **Wake** | Any button press (GPIO low-level interrupt) |
| **Wi-Fi** | Soft-AP `ReFocus-Cam` / `antigravity` |
| **Camera** | OV5647 via MIPI-CSI (JPEG native output) |
| **Storage** | SD/MMC 4-bit bus, FAT filesystem at `/sdcard` |

---

## REST API (PWA-compatible)

| Method | Path | Response |
|--------|------|----------|
| `GET`  | `/api/handshake` | `{"status":"ok","total":<N>}` |
| `GET`  | `/api/images` | JSON array of `{"filename","size"}` objects |
| `GET`  | `/api/image/<filename>` | Raw JPEG bytes |
| `OPTIONS` | `/*` | `200 OK` (CORS preflight) |

All responses include `Access-Control-Allow-Origin: *` so the PWA can connect
from any origin.

---

## Project Structure

```
ReFocus_Camera/
├── CMakeLists.txt          ← project root
├── sdkconfig.defaults      ← ESP32-P4 minimal config
└── main/
    ├── CMakeLists.txt      ← component registration
    ├── config.h            ← all pin / path / timing constants ← edit here
    ├── main.c              ← app_main, button ISR, tasks
    ├── camera.c / .h       ← V4L2 camera init & JPEG capture
    ├── sdcard.c / .h       ← SDMMC mount, file helpers
    ├── wifi_ap.c / .h      ← Soft-AP init
    └── webserver.c / .h    ← esp_http_server REST handlers
```

---

## Quick Start

### 1. Prerequisites

- ESP-IDF **v5.3+** targeting `esp32p4`
- Waveshare ESP32-P4-WiFi6 board
- SD card (FAT32 formatted)
- OV5647 camera module connected to the MIPI-CSI FPC connector

### 2. Configure

Edit `main/config.h` to adjust:

```c
#define WIFI_AP_SSID   "ReFocus-Cam"   // AP network name
#define WIFI_AP_PASS   "antigravity"   // AP password (min 8 chars)
#define IDLE_SLEEP_MS  10000           // idle-before-sleep in ms
#define BOOT_BUTTON_GPIO 0             // GPIO for the capture button
```

### 3. Build & Flash

```bash
cd firmware/ReFocus_Camera
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 4. Connect

1. Connect your phone/laptop to Wi-Fi network **ReFocus-Cam**.
2. Open the ReFocus PWA and point it to `http://192.168.4.1`.
3. Press **BOOT** on the board to capture photos.

---

## Hardware Notes

### SD Card (SDMMC slot 0 — 4-bit bus)

The Waveshare board exposes the SD card on the native SDMMC pins. No
additional configuration is needed; the driver uses the chip defaults.

### Camera (MIPI-CSI)

The OV5647 is configured for **JPEG native output** at 800×1280 @ 50 fps via
`CONFIG_CAMERA_OV5647_MIPI_RAW8_800x1280_50FPS`.  If your sensor variant only
outputs RAW bayer data you will need to add a hardware JPEG encoder step in
`camera.c` (see `17_simple_video_server` for reference).

### BOOT Button

GPIO 0 has a built-in pull-up on most ESP32 boards. The ISR fires on the
**falling edge** (button pressed) and debounces at 300 ms.

### Light-Sleep

During light-sleep the CPU is halted but SRAM is preserved.  The Wi-Fi radio
continues to handle beacon traffic (the AP stays discoverable). The device
wakes on a **low-level** signal on GPIO 0 (button held down).

---

## Extending the Firmware

| Goal | Where to change |
|------|----------------|
| Different camera resolution | `sdkconfig.defaults` — change `CONFIG_CAMERA_OV5647_*` |
| Different sensor (e.g. OV2640) | `sdkconfig.defaults` + `camera.c` pixel format |
| Station mode instead of AP | Replace `wifi_ap.c` with the `10_wifistation` pattern |
| Longer idle timeout | `config.h` → `IDLE_SLEEP_MS` |
| Deep-sleep (full power-off) | Replace `esp_light_sleep_start()` with `esp_deep_sleep_start()` |
