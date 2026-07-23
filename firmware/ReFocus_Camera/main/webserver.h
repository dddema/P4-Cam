#pragma once
#include "esp_err.h"

/**
 * @brief  Start the HTTP server.
 *
 * Registers all REST endpoints that the ReFocus PWA expects:
 *
 *   GET  /api/handshake          → {"status":"ok","total":<N>}
 *   GET  /api/images             → JSON array of image descriptors
 *   GET  /api/image/<filename>   → raw JPEG bytes
 *   OPTIONS *                    → CORS preflight (200 OK)
 *
 * @return ESP_OK on success.
 */
esp_err_t webserver_start(void);

/**
 * @brief  Stop and free the HTTP server.
 */
void webserver_stop(void);
