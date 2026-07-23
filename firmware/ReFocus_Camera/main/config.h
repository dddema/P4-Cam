#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ─── SD CARD ──────────────────────────────────────────────────────────────── */
/* Waveshare ESP32-P4-WiFi6 exposes SD/MMC on the standard SDMMC slot 0 pins. */
#define SDCARD_MOUNT_POINT  "/sdcard"
#define SDCARD_BASE_DIR     "/sdcard"
#define SDCARD_IMG_DIR      "/sdcard"  /* photos saved directly to root */
#define IMG_FILE_PREFIX     "img_"
#define IMG_FILE_EXT        ".jpg"
#define IMG_MAX_FILES       999

/* ─── CAMERA ────────────────────────────────────────────────────────────────── */
#define CAMERA_DEVICE       "/dev/video0"       /* V4L2 CSI camera device      */
#define CAMERA_WIDTH        1920
#define CAMERA_HEIGHT       1080
#define CAM_PWDN_GPIO       -1                  /* Set physical PWDN GPIO pin (e.g. 50) if wired, or -1 */


/* ─── GPIO ──────────────────────────────────────────────────────────────────── */
#define BOOT_BUTTON_GPIO    35   /* BOOT / FLASH button on Waveshare ESP32-P4-WiFi6 */

/* ─── TIMING ────────────────────────────────────────────────────────────────── */
#define IDLE_SLEEP_MS       10000   /* go to light-sleep after 10 s of inactivity */

/* ─── Wi-Fi AP ──────────────────────────────────────────────────────────────── */
#define WIFI_AP_SSID        "ReFocus-Cam"
#define WIFI_AP_PASS        "antigravity"   /* min 8 chars for WPA2              */
#define WIFI_AP_CHANNEL     6
#define WIFI_AP_MAX_CONN    4
