/*
 * ReFocus Camera — Camera module
 *
 * Uses the esp_video / V4L2 API present in Espressif's ESP-IDF fork for the
 * ESP32-P4 to open the MIPI-CSI sensor, stream frames in JPEG format, and
 * expose a simple capture_jpeg() function to the rest of the application.
 *
 * The sensor is assumed to output JPEG natively (e.g. OV5647 configured for
 * JPEG mode).  If the sensor only supports RAW/RGB, you would need to add a
 * software JPEG encoder step here (see example 17_simple_video_server for a
 * reference using the hardware JPEG encoder).
 */

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "driver/jpeg_encode.h"
#include "driver/jpeg_types.h"
#include "esp_video_isp_ioctl.h"

#include "config.h"
#include "camera.h"

static const char *TAG = "camera";

/* ─── module state ─────────────────────────────────────────────────── */
#define CAM_BUF_COUNT  3

typedef struct {
    uint8_t *ptr;
    size_t   size;
} cam_buf_t;

static int       s_fd        = -1;
static cam_buf_t s_bufs[CAM_BUF_COUNT];
static bool      s_streaming = false;
static uint32_t  s_width     = CAMERA_WIDTH;
static uint32_t  s_height    = CAMERA_HEIGHT;

/* ─── helpers ──────────────────────────────────────────────────────── */
static esp_err_t start_stream(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: %d", errno);
        return ESP_FAIL;
    }
    s_streaming = true;
    return ESP_OK;
}

static esp_err_t stop_stream(void)
{
    if (!s_streaming) return ESP_OK;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    s_streaming = false;
    return ESP_OK;
}

/* ─── public API ────────────────────────────────────────────────────── */
esp_err_t camera_init(void)
{
    /* 1. Initialise the Espressif video subsystem (registers the V4L2 driver) */
    esp_video_init_csi_config_t csi_cfg[] = {{
        .sccb_config = {
            .init_sccb         = true,   /* let esp_video initialise the I2C bus */
            .i2c_config = {
                .port    = 0,            /* I2C port 0 */
                .scl_pin = 8,            /* OV5647 SCCB SCL — matches example sdkconfig */
                .sda_pin = 7,            /* OV5647 SCCB SDA */
            },
            .freq = 100000,              /* 100 kHz I2C clock */
        },
        .reset_pin = -1,
        .pwdn_pin  = CAM_PWDN_GPIO,
    }};
    esp_video_init_config_t vcfg = { .csi = csi_cfg };

    esp_err_t ret = esp_video_init(&vcfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. Open the V4L2 device */
    s_fd = open(CAMERA_DEVICE, O_RDWR);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed: %d", CAMERA_DEVICE, errno);
        return ESP_FAIL;
    }

    /* 3. Query capabilities (for logging) */
    struct v4l2_capability cap;
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "Camera driver: %s  card: %s", cap.driver, cap.card);
    }

    /* 4. Set RGB565 pixel format with custom resolution request */
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        goto err_close;
    }

    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.width       = CAMERA_WIDTH;
    fmt.fmt.pix.height      = CAMERA_HEIGHT;
    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "failed to set format to RGB565 with resolution %dx%d", CAMERA_WIDTH, CAMERA_HEIGHT);
        goto err_close;
    }

    /* Get the finalized width and height after format negotiation */
    s_width = fmt.fmt.pix.width;
    s_height = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "Camera format set to RGB565 (%ldx%ld)", s_width, s_height);

    /* 5. Request MMAP buffers */
    struct v4l2_requestbuffers reqbuf = {
        .count  = CAM_BUF_COUNT,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &reqbuf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        goto err_close;
    }

    /* 6. Query, mmap, and queue each buffer */
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index  = i,
        };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed", i);
            goto err_close;
        }

        s_bufs[i].size = buf.length;
        s_bufs[i].ptr  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, s_fd, buf.m.offset);
        if (s_bufs[i].ptr == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%d] failed", i);
            goto err_close;
        }

        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed", i);
            goto err_close;
        }
    }

    /* 7. Start streaming */
    ret = start_stream();
    if (ret != ESP_OK) goto err_close;

    /* 8. Configure ISP adjustments on the ISP device node "/dev/video20" */
    int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (isp_fd >= 0) {
        struct v4l2_ext_controls ctrls = {0};
        struct v4l2_ext_control ctrl[3] = {0};

        ctrl[0].id = V4L2_CID_CONTRAST;
        ctrl[0].value = 128; // 0.5 * 128 = 64 (Lowered from 90 because 0.7 was too high)

        ctrl[1].id = V4L2_CID_SATURATION;
        ctrl[1].value = 128; // 0.5 * 128 = 64 (Lowered from 90 because 0.7 was too high)

        esp_video_isp_bf_t bf = {
            .enable = false,
        };
        ctrl[2].id = V4L2_CID_USER_ESP_ISP_BF;
        ctrl[2].ptr = &bf;
        ctrl[2].size = sizeof(bf);

        ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
        ctrls.count = 3;
        ctrls.controls = ctrl;

        if (ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
            ESP_LOGW(TAG, "Failed to set Contrast/Saturation/Denoise settings on ISP device: %d", errno);
        } else {
            ESP_LOGI(TAG, "Successfully set Contrast=0.5, Saturation=0.5, Denoise=OFF on ISP device");
        }

        /* Verification read-back check */
        struct v4l2_ext_controls ctrls_get = {0};
        struct v4l2_ext_control ctrl_get[3] = {0};
        esp_video_isp_bf_t bf_get = {0};

        ctrl_get[0].id = V4L2_CID_CONTRAST;
        ctrl_get[1].id = V4L2_CID_SATURATION;
        ctrl_get[2].id = V4L2_CID_USER_ESP_ISP_BF;
        ctrl_get[2].ptr = &bf_get;
        ctrl_get[2].size = sizeof(bf_get);

        ctrls_get.ctrl_class = V4L2_CTRL_CLASS_USER;
        ctrls_get.count = 3;
        ctrls_get.controls = ctrl_get;

        if (ioctl(isp_fd, VIDIOC_G_EXT_CTRLS, &ctrls_get) == 0) {
            ESP_LOGI(TAG, "🔍 Verification read-back from ISP: Contrast=%ld, Saturation=%ld, Denoise=%s",
                     ctrl_get[0].value, ctrl_get[1].value, bf_get.enable ? "ON" : "OFF");
        } else {
            ESP_LOGW(TAG, "Failed to read back ISP controls: %d", errno);
        }

        close(isp_fd);
    } else {
        ESP_LOGE(TAG, "Failed to open ISP device %s: %d", ESP_VIDEO_ISP1_DEVICE_NAME, errno);
    }

    ESP_LOGI(TAG, "Camera ready");
    return ESP_OK;

err_close:
    close(s_fd);
    s_fd = -1;
    return ESP_FAIL;
}

/* ─── capture ───────────────────────────────────────────────────────── */
esp_err_t camera_capture_jpeg(uint8_t **out_buf, size_t *out_len)
{
    if (s_fd < 0 || !s_streaming) {
        ESP_LOGE(TAG, "Camera not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain (CAM_BUF_COUNT - 1) older frames from the V4L2 queue so the next dequeue gets the freshest frame */
    struct v4l2_buffer buf;
    for (int i = 0; i < (CAM_BUF_COUNT - 1); i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) == 0) {
            ioctl(s_fd, VIDIOC_QBUF, &buf);
        }
    }

    /* Dequeue the newest fresh frame */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed: %d", errno);
        return ESP_FAIL;
    }

    uint8_t *raw_rgb = s_bufs[buf.index].ptr;
    size_t raw_rgb_size = buf.bytesused;

    ESP_LOGI(TAG, "Frame captured: buf[%d] %zu bytes raw RGB565", buf.index, raw_rgb_size);

    /* Initialize HW JPEG encoder */
    jpeg_encoder_handle_t jpeg_enc = NULL;
    jpeg_encode_engine_cfg_t eng_cfg = {
        .timeout_ms = 5000,
    };
    esp_err_t ret = jpeg_new_encoder_engine(&eng_cfg, &jpeg_enc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder engine (%s)", esp_err_to_name(ret));
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        return ret;
    }

    jpeg_encode_cfg_t enc_cfg = {
        .width = s_width,
        .height = s_height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 85,
    };

    size_t raw_size = s_width * s_height * 2;
    size_t allocated_size = 0;
    jpeg_encode_memory_alloc_cfg_t alloc_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    uint8_t *jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(raw_size, &alloc_cfg, &allocated_size);
    if (!jpeg_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG encoder memory");
        jpeg_del_encoder_engine(jpeg_enc);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        return ESP_ERR_NO_MEM;
    }

    uint32_t jpeg_size = 0;
    ret = jpeg_encoder_process(jpeg_enc, &enc_cfg, raw_rgb, raw_rgb_size, jpeg_out_buf, allocated_size, &jpeg_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encoding failed (%s)", esp_err_to_name(ret));
        free(jpeg_out_buf);
        jpeg_del_encoder_engine(jpeg_enc);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        return ret;
    }

    /* Copy to standard malloc'd buffer so it can be handled normally */
    uint8_t *copy = malloc(jpeg_size);
    if (!copy) {
        ESP_LOGE(TAG, "Failed to allocate copy buffer");
        free(jpeg_out_buf);
        jpeg_del_encoder_engine(jpeg_enc);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, jpeg_out_buf, jpeg_size);

    free(jpeg_out_buf);
    jpeg_del_encoder_engine(jpeg_enc);

    /* Re-queue buffer for the next frame */
    if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGW(TAG, "VIDIOC_QBUF failed after capture");
    }

    *out_buf = copy;
    *out_len = jpeg_size;
    return ESP_OK;
}


/* ─── deinit ────────────────────────────────────────────────────────── */
void camera_deinit(void)
{
    stop_stream();
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        if (s_bufs[i].ptr && s_bufs[i].ptr != MAP_FAILED) {
            munmap(s_bufs[i].ptr, s_bufs[i].size);
            s_bufs[i].ptr = NULL;
        }
    }
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
    ESP_LOGI(TAG, "Camera deinitialized");
}
