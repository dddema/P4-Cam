#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief  Mount the SD card via SDMMC (4-bit bus, slot 0).
 * @return ESP_OK on success.
 */
esp_err_t sdcard_mount(void);

/**
 * @brief  Unmount the SD card and disable the SDMMC peripheral.
 */
void sdcard_unmount(void);

/**
 * @brief  Count how many IMG_FILE_PREFIX*.jpg files are on the card.
 * @return Number of image files found (0 if none or card not mounted).
 */
int sdcard_count_images(void);

/**
 * @brief  Build the next unique sequential file path (e.g. /sdcard/img_001.jpg).
 * @param  out_path   Output buffer (must be at least 64 bytes).
 * @param  out_index  Receives the numeric index used.
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED if the counter overflows IMG_MAX_FILES.
 */
esp_err_t sdcard_next_image_path(char *out_path, int *out_index);

/**
 * @brief  Write a raw JPEG buffer to the given path on the SD card.
 * @param  path   Full absolute path (must be in SDCARD_BASE_DIR).
 * @param  data   Pointer to JPEG bytes.
 * @param  len    Number of bytes.
 * @return ESP_OK on success.
 */
esp_err_t sdcard_write_jpeg(const char *path, const uint8_t *data, size_t len);
