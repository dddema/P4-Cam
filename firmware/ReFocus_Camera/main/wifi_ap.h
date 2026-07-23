#pragma once
#include "esp_err.h"

/**
 * @brief  Start the Wi-Fi Access Point.
 *
 * The AP uses the SSID/password defined in config.h.  Clients connect to
 * 192.168.4.1 (the default SoftAP IP).
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_start(void);

/**
 * @brief  Stop and deinitialise the Wi-Fi AP.
 */
void wifi_ap_stop(void);
