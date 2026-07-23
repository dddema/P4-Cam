#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief  Open the CSI camera, configure buffers, and start streaming.
 * @return ESP_OK on success.
 */
esp_err_t camera_init(void);

/**
 * @brief  Capture a single JPEG frame.
 *
 * The returned buffer is heap-allocated; the caller must free() it when done.
 *
 * @param[out] out_buf   Receives a pointer to the JPEG data.
 * @param[out] out_len   Receives the number of bytes.
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t camera_capture_jpeg(uint8_t **out_buf, size_t *out_len);

/**
 * @brief  Stop streaming and release all camera resources.
 */
void camera_deinit(void);
