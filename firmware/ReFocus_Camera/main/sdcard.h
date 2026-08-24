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

/**
 * @brief  Get the next auto-incrementing index for a default spool folder.
 * @return Next spool index.
 */
int sdcard_get_next_spool_index(void);

/**
 * @brief  Move all img_*.jpg files from the root of the SD card into a new
 *         subdirectory in "/sdcard/developed/<name>/" to mark them as developed.
 * @param  requested_name  Custom folder name (if NULL or empty, defaults to "spool_N").
 * @return ESP_OK on success.
 */
esp_err_t sdcard_wipe_spool(const char *requested_name);

/**
 * @brief  Delete an archived spool directory and all its files from "/sdcard/developed/<name>/".
 * @param  spool_name  Name of the spool folder to delete.
 * @return ESP_OK on success.
 */
esp_err_t sdcard_delete_spool(const char *spool_name);

/**
 * @brief  Rename an archived spool folder from old_name to new_name.
 * @param  old_name  Original directory name (e.g. "spool_monaco").
 * @param  new_name  New directory name (will be sanitized and prepended with "spool_").
 * @return ESP_OK on success.
 */
esp_err_t sdcard_rename_spool(const char *old_name, const char *new_name);


