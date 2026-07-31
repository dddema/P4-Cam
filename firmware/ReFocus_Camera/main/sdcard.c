/*
 * ReFocus Camera — SD Card module
 *
 * Mounts the Waveshare ESP32-P4-WiFi6 on-board SD/MMC card via the native
 * SDMMC peripheral (4-bit bus, slot 0).  All image files are stored directly
 * in the root of the FAT partition as img_NNN.jpg.
 */

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "config.h"
#include "sdcard.h"
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "tinyusb_default_config.h"

static const char *TAG = "sdcard";

/* ─── module state ─────────────────────────────────────────────────── */
static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle = NULL;
static tinyusb_msc_storage_handle_t s_msc_storage_handle = NULL;

#define EPNUM_MSC               1
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN  = 0x80,
    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(s_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // Espressif VID
    .idProduct = 0x4002, // Product ID for MSC
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const uint8_t s_msc_fs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};

static const uint8_t s_msc_hs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};
#endif

static const char *s_string_descriptor[] = {
    (const char[]) { 0x09, 0x04 }, // English language ID
    "ReFocus",                     // Manufacturer
    "ReFocus Camera Spool",        // Product
    "654321",                      // Serial number
    "Camera MSC spool",            // MSC interface name
};

static void storage_mount_changed_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        ESP_LOGI(TAG, "Storage mount start: unmounting local filesystem...");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "Storage mount completed. Active mount to APP (filesystem): %s",
                 (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) ? "YES" : "NO");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "Storage mount failed!");
        break;
    default:
        break;
    }
}



#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
static esp_err_t sdmmc_host_init_dummy(void)
{
    return ESP_OK;
}

static esp_err_t sdmmc_host_deinit_dummy(void)
{
    return ESP_OK;
}
#endif

/* ─── mount ────────────────────────────────────────────────────────── */
/* ─── mount ────────────────────────────────────────────────────────── */
esp_err_t sdcard_mount(void)
{
    if (s_card != NULL) {
        ESP_LOGW(TAG, "Already mounted");
        return ESP_OK;
    }

    /* Enable power to the SD card on Waveshare board via on-chip LDO Channel 4 */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl_handle);
    if (pwr_err == ESP_OK) {
        ESP_LOGI(TAG, "Initialized on-chip LDO Channel 4 for SD card power");
    } else {
        ESP_LOGW(TAG, "Failed to create on-chip LDO power control handle (%s)", esp_err_to_name(pwr_err));
    }

    /* SDMMC host – default 20 MHz, internal pull-ups enabled */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0; /* Waveshare SD card slot is on slot 0 */
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif
    if (s_pwr_ctrl_handle) {
        host.pwr_ctrl_handle = s_pwr_ctrl_handle;
    }

    /* Slot 0: 4-bit bus; map pins explicitly to match Waveshare board layout */
    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width  = 4;
    slot_cfg.clk    = 43;
    slot_cfg.cmd    = 44;
    slot_cfg.d0     = 39;
    slot_cfg.d1     = 40;
    slot_cfg.d2     = 41;
    slot_cfg.d3     = 42;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* Allocate memory for the card structure */
    s_card = malloc(sizeof(sdmmc_card_t));
    if (s_card == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for SD card struct");
        goto err_ldo;
    }

    /* Call the host init and slot init manually (instead of esp_vfs_fat_sdmmc_mount doing it) */
    esp_err_t ret = (*host.init)();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Host init failed (%s)", esp_err_to_name(ret));
        goto err_free;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Host init slot failed (%s)", esp_err_to_name(ret));
        goto err_free;
    }

    ret = sdmmc_card_init(&host, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed (%s)", esp_err_to_name(ret));
        goto err_free;
    }

    ESP_LOGI(TAG, "Initialising TinyUSB MSC dynamic storage adapter ...");
    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = SDCARD_MOUNT_POINT,
            .config.max_files = 8,
            .do_not_format = true,
            .format_flags = 0,
        },
        .medium = {
            .card = s_card,
        },
    };

    ret = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_msc_storage_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MSC SDMMC storage: %s", esp_err_to_name(ret));
        goto err_free;
    }

    tinyusb_msc_set_storage_callback(storage_mount_changed_cb, NULL);

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0; /* Force internal FS PHY connected to Waveshare USB port */
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = s_msc_fs_configuration_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_msc_hs_configuration_descriptor;
    tusb_cfg.descriptor.qualifier = &s_device_qualifier;
#endif

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        goto err_msc;
    }

    /* Mount the storage onto the application filesystem (/sdcard) */
    ret = tinyusb_msc_set_storage_mount_point(s_msc_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card locally: %s", esp_err_to_name(ret));
        goto err_usb;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted locally at %s and dynamic USB MSC active!", SDCARD_MOUNT_POINT);
    return ESP_OK;

err_usb:
    tinyusb_driver_uninstall();
err_msc:
    if (s_msc_storage_handle) {
        tinyusb_msc_delete_storage(s_msc_storage_handle);
        s_msc_storage_handle = NULL;
    }
err_free:
    free(s_card);
    s_card = NULL;
err_ldo:
    if (s_pwr_ctrl_handle != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        s_pwr_ctrl_handle = NULL;
    }
    return ESP_FAIL;
}

/* ─── unmount ──────────────────────────────────────────────────────── */
void sdcard_unmount(void)
{
    if (s_card == NULL) return;

    tinyusb_driver_uninstall();
    if (s_msc_storage_handle != NULL) {
        tinyusb_msc_delete_storage(s_msc_storage_handle);
        s_msc_storage_handle = NULL;
    }

    free(s_card);
    s_card = NULL;

    if (s_pwr_ctrl_handle != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        s_pwr_ctrl_handle = NULL;
    }
    ESP_LOGI(TAG, "SD card and USB MSC unmounted");
}

/* ─── image-file helpers ───────────────────────────────────────────── */
int sdcard_count_images(void)
{
    DIR *dir = opendir(SDCARD_IMG_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG) continue;
        const char *dot = strrchr(ent->d_name, '.');
        if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

esp_err_t sdcard_next_image_path(char *out_path, int *out_index)
{
    /* Find the highest existing index and increment */
    DIR *dir = opendir(SDCARD_IMG_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open image directory");
        return ESP_FAIL;
    }

    int max_idx = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG) continue;
        int idx = 0;
        if (sscanf(ent->d_name, IMG_FILE_PREFIX "%d" IMG_FILE_EXT, &idx) == 1) {
            if (idx > max_idx) max_idx = idx;
        }
    }
    closedir(dir);

    int next = max_idx + 1;
    if (next > IMG_MAX_FILES) {
        ESP_LOGE(TAG, "Image index overflow (max %d)", IMG_MAX_FILES);
        return ESP_ERR_NOT_SUPPORTED;
    }

    snprintf(out_path, 64, "%s/" IMG_FILE_PREFIX "%03d" IMG_FILE_EXT,
             SDCARD_IMG_DIR, next);
    if (out_index) *out_index = next;
    return ESP_OK;
}

esp_err_t sdcard_write_jpeg(const char *path, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "Writing %zu bytes → %s", len, path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen failed for %s", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Incomplete write (%zu/%zu)", written, len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Saved %s (%zu bytes)", path, len);
    return ESP_OK;
}

int sdcard_get_next_spool_index(void)
{
    struct stat st;
    if (stat("/sdcard/developed", &st) != 0) {
        return 1;
    }
    DIR *dev_dir = opendir("/sdcard/developed");
    if (!dev_dir) {
        return 1;
    }
    int max_spool_idx = 0;
    struct dirent *ent;
    while ((ent = readdir(dev_dir)) != NULL) {
        int idx = 0;
        if (sscanf(ent->d_name, "spool_%d", &idx) == 1) {
            if (idx > max_spool_idx) max_spool_idx = idx;
        }
    }
    closedir(dev_dir);
    return max_spool_idx + 1;
}

esp_err_t sdcard_wipe_spool(const char *requested_name)
{
    /* 1. Create SDCARD_IMG_DIR "/developed" if it doesn't exist */
    struct stat st;
    if (stat("/sdcard/developed", &st) != 0) {
        if (mkdir("/sdcard/developed", 0777) != 0) {
            ESP_LOGE(TAG, "Failed to create /sdcard/developed directory");
            return ESP_FAIL;
        }
    }

    /* 2. Determine target folder name, applying auto-incrementing suffix to avoid collisions */
    char folder_name[128] = {0};
    if (requested_name && requested_name[0] != '\0') {
        const char *p = requested_name;
        if (strncasecmp(p, "spool_", 6) == 0) {
            p += 6;
        }
        snprintf(folder_name, sizeof(folder_name), "spool_%s", p);
        
        /* Lowercase and sanitize spaces/special characters */
        for (int i = 0; folder_name[i] != '\0'; i++) {
            char c = folder_name[i];
            if (c >= 'A' && c <= 'Z') {
                folder_name[i] = c + 32;
            } else if (c == ' ' || c == '-' || c == '/' || c == '\\') {
                folder_name[i] = '_';
            }
        }
    } else {
        int next_spool = sdcard_get_next_spool_index();
        snprintf(folder_name, sizeof(folder_name), "spool_%d", next_spool);
    }

    char spool_path[256];
    snprintf(spool_path, sizeof(spool_path), "/sdcard/developed/%s", folder_name);

    /* If folder already exists, find a non-colliding name by appending "_N" */
    int suffix = 1;
    while (stat(spool_path, &st) == 0) {
        if (requested_name && requested_name[0] != '\0') {
            snprintf(folder_name, sizeof(folder_name), "%s_%d", requested_name, suffix);
        } else {
            int next_spool = sdcard_get_next_spool_index();
            snprintf(folder_name, sizeof(folder_name), "spool_%d_%d", next_spool, suffix);
        }
        snprintf(spool_path, sizeof(spool_path), "/sdcard/developed/%s", folder_name);
        suffix++;
    }

    /* 3. Create the spool folder */
    if (mkdir(spool_path, 0777) != 0) {
        ESP_LOGE(TAG, "Failed to create directory %s", spool_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Created spool archive directory: %s", spool_path);

    /* 4. Scan the root directory /sdcard and move all img_*.jpg files into the new folder */
    DIR *root_dir = opendir(SDCARD_IMG_DIR);
    if (!root_dir) {
        ESP_LOGE(TAG, "Cannot open image root directory");
        return ESP_FAIL;
    }

    int moved_count = 0;
    struct dirent *ent;
    while ((ent = readdir(root_dir)) != NULL) {
        if (ent->d_type != DT_REG) continue;
        int img_idx = 0;
        if (sscanf(ent->d_name, IMG_FILE_PREFIX "%d" IMG_FILE_EXT, &img_idx) == 1) {
            char old_path[512];
            char new_path[512];
            snprintf(old_path, sizeof(old_path), "%s/%s", SDCARD_IMG_DIR, ent->d_name);
            snprintf(new_path, sizeof(new_path), "%s/%s", spool_path, ent->d_name);
            if (rename(old_path, new_path) == 0) {
                moved_count++;
            } else {
                ESP_LOGE(TAG, "Failed to move %s to %s", old_path, new_path);
            }
        }
    }
    closedir(root_dir);

    ESP_LOGI(TAG, "Spool wiped successfully! Moved %d images to %s", moved_count, spool_path);
    return ESP_OK;
}

esp_err_t sdcard_delete_spool(const char *spool_name)
{
    if (!spool_name || spool_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    
    char spool_path[256];
    snprintf(spool_path, sizeof(spool_path), "/sdcard/developed/%s", spool_name);
    
    // Safety check: prevent directory traversal
    if (strstr(spool_name, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    DIR *dir = opendir(spool_path);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open directory for deletion: %s", spool_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", spool_path, ent->d_name);
            if (unlink(filepath) != 0) {
                ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
            }
        }
    }
    closedir(dir);
    
    if (rmdir(spool_path) != 0) {
        ESP_LOGE(TAG, "Failed to remove directory: %s", spool_path);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Deleted spool directory successfully: %s", spool_path);
    return ESP_OK;
}

