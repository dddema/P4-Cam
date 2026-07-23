/*
 * ReFocus Camera — main application
 *
 * Waveshare ESP32-P4-WiFi6 firmware
 *
 * Behaviour:
 *   • On boot  — mount SD card, init camera, start Wi-Fi AP and HTTP server.
 *   • Boot btn — single press → capture JPEG, save to SD card.
 *   • Idle     — after IDLE_SLEEP_MS of no button activity → light-sleep.
 *                Any GPIO interrupt (button press) wakes the device.
 *
 * REST API (compatible with the existing ReFocus PWA):
 *   GET  /api/handshake           {"status":"ok","total":<N>}
 *   GET  /api/images              JSON array of image descriptors
 *   GET  /api/image/<filename>    raw JPEG bytes
 *   OPTIONS *                     CORS preflight
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "config.h"
#include "sdcard.h"
#include "camera.h"
#include "wifi_ap.h"
#include "webserver.h"

static const char *TAG = "main";

/* ─── module state ─────────────────────────────────────────────────── */

/* Queue receives a '1' when the boot button fires an ISR */
static QueueHandle_t s_btn_queue = NULL;

/* Timestamp of the last user interaction (ms since boot) */
static volatile int64_t s_last_activity_ms = 0;

/* ─── boot-button ISR ───────────────────────────────────────────────── */
static void IRAM_ATTR btn_isr_handler(void *arg)
{
    uint8_t sig = 1;
    xQueueSendFromISR(s_btn_queue, &sig, NULL);
}

/* ─── capture task ──────────────────────────────────────────────────── */
/*
 * Dedicated FreeRTOS task that waits for button events and drives the
 * capture → save pipeline.  Running in its own task keeps the ISR handler
 * as short as possible and lets us call blocking file-I/O safely.
 */
static void capture_task(void *arg)
{
    uint8_t sig;
    while (1) {
        /* Block forever until a button event arrives */
        if (xQueueReceive(s_btn_queue, &sig, portMAX_DELAY) != pdTRUE) continue;

        /* Debounce: ignore events that arrive < 300 ms after the last one */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - s_last_activity_ms) < 300) {
            ESP_LOGD(TAG, "Button debounced");
            continue;
        }
        s_last_activity_ms = now_ms;

        ESP_LOGI(TAG, "📸 Boot button pressed — capturing …");

        /* Capture JPEG from camera instantly with zero shutter lag */
        uint8_t *jpeg_buf = NULL;
        size_t   jpeg_len = 0;
        esp_err_t ret = camera_capture_jpeg(&jpeg_buf, &jpeg_len);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Capture failed");
            continue;
        }

        /* 2. Build the next sequential file path */
        char path[64];
        int  idx = 0;
        ret = sdcard_next_image_path(path, &idx);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not build image path");
            free(jpeg_buf);
            continue;
        }

        /* 3. Save to SD card */
        ret = sdcard_write_jpeg(path, jpeg_buf, jpeg_len);
        free(jpeg_buf);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Saved %s", path);
        } else {
            ESP_LOGE(TAG, "Failed to save image");
        }
    }
}

/* ─── idle / sleep monitor ──────────────────────────────────────────── */
/*
 * Lightweight task that checks every second whether the device has been idle
 * long enough to enter light-sleep.  Light-sleep keeps the SRAM contents
 * intact and allows the GPIO interrupt to wake the device instantly.
 *
 * Note: The Wi-Fi AP is intentionally left running during light-sleep so that
 * the PWA can still open connections (the radio wakes automatically for beacon
 * transmission).  If power consumption is a concern, add esp_wifi_set_ps()
 * with WIFI_PS_MIN_MODEM here.
 */
static void sleep_monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int64_t now_ms  = esp_timer_get_time() / 1000;
        int64_t idle_ms = now_ms - s_last_activity_ms;

        if (idle_ms >= IDLE_SLEEP_MS) {
            ESP_LOGI(TAG, "💤 Idle for %lld ms — entering light-sleep …", idle_ms);

            /* Configure wake-up source: LOW level on BOOT button GPIO */
            gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();

            esp_light_sleep_start();  /* returns here on wake */

            /* Woke up — reset idle timer and drain any queued ISR events */
            s_last_activity_ms = esp_timer_get_time() / 1000;
            uint8_t dummy;
            while (xQueueReceive(s_btn_queue, &dummy, 0) == pdTRUE) { /* flush */ }

            ESP_LOGI(TAG, "☀️  Woke from light-sleep");
        }
    }
}

/* ─── GPIO init ─────────────────────────────────────────────────────── */
static void gpio_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE, /* falling edge = button pressed */
    };
    gpio_config(&io_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, btn_isr_handler, NULL);

    ESP_LOGI(TAG, "GPIO %d configured as boot/capture button", BOOT_BUTTON_GPIO);
}

/* ─── app_main ──────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "════ ReFocus Camera firmware booting ════");

    /* ── NVS (required by Wi-Fi) ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* ── SD card ── */
    ret = sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed — images will not be saved");
    }

    /* ── Camera ── */
    /* Keep camera initialized constantly from boot to eliminate shutter lag */
    ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed — capture will be disabled");
    }

    /* ── Wi-Fi Access Point ── */
    ESP_ERROR_CHECK(wifi_ap_start());

    /* Give the AP a moment to come up before starting HTTP */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ── HTTP server ── */
    ESP_ERROR_CHECK(webserver_start());

    /* ── GPIO / button ── */
    s_btn_queue = xQueueCreate(4, sizeof(uint8_t));
    configASSERT(s_btn_queue);
    gpio_init();

    /* ── Seed the idle timer ── */
    s_last_activity_ms = esp_timer_get_time() / 1000;

    /* ── Background tasks ── */
    xTaskCreate(capture_task,       "capture",       8192, NULL, 5, NULL);
    /* Note: sleep_monitor_task is kept disabled to prevent light-sleep from shutting down 
       the SDIO clock and dropping the Wi-Fi remote link. */
    /* xTaskCreate(sleep_monitor_task, "sleep_monitor", 4096, NULL, 3, NULL); */


    ESP_LOGI(TAG, "════ ReFocus Camera ready — AP: \"%s\" ════", WIFI_AP_SSID);
    ESP_LOGI(TAG, "Connect to the AP and open http://192.168.4.1");
}
