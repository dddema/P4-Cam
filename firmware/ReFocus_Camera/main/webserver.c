/*
 * ReFocus Camera — HTTP Web Server module
 *
 * Implements the same REST API that the existing Arduino firmware exposed so
 * that the PWA works without any changes:
 *
 *   GET  /api/handshake         {"status":"ok","total":<N>}
 *   GET  /api/images            JSON array: [{"filename":"img_001.jpg","size":...}, …]
 *   GET  /api/image/<name>      raw JPEG bytes for one image
 *   OPTIONS *                   CORS preflight
 *
 * CORS headers are added to every response so the PWA (served from any origin)
 * can reach the camera directly.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "config.h"
#include "sdcard.h"
#include "webserver.h"
#include "cJSON.h"

static const char *TAG = "webserver";

static httpd_handle_t s_server = NULL;

/* ─── CORS helper ───────────────────────────────────────────────────── */
static void add_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
}

/* ─── OPTIONS handler (all methods) ────────────────────────────────── */
static esp_err_t options_handler(httpd_req_t *req)
{
    add_cors(req);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"UTF-8\">\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, viewport-fit=cover\">\n"
"  <title>REFOCUS // CAMERA HOST</title>\n"
"  <link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n"
"  <link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n"
"  <link href=\"https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;700&display=swap\" rel=\"stylesheet\">\n"
"  <style>\n"
"    :root {\n"
"      --bg-color: #f7f4ef;\n"
"      --card-bg: rgba(255, 255, 255, 0.7);\n"
"      --accent: rgb(217, 119, 43);\n"
"      --text: #2f2520;\n"
"      --text-muted: #8a7c72;\n"
"      --border: rgba(217, 119, 43, 0.15);\n"
"      --font-sans: 'Outfit', -apple-system, sans-serif;\n"
"    }\n"
"    * { box-sizing: border-box; }\n"
"    body {\n"
"      margin: 0;\n"
"      padding: 0;\n"
"      width: 100vw;\n"
"      height: 100vh;\n"
"      background-color: var(--bg-color);\n"
"      color: var(--text);\n"
"      font-family: var(--font-sans);\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      align-items: center;\n"
"      justify-content: center;\n"
"    }\n"
"    body::before {\n"
"      content: \"\";\n"
"      position: fixed;\n"
"      top: 0; left: 0; width: 100%; height: 100%;\n"
"      opacity: 0.035;\n"
"      pointer-events: none;\n"
"      z-index: 9999;\n"
"      background-image: url(\"data:image/svg+xml,%3Csvg viewBox='0 0 200 200' xmlns='http://www.w3.org/2000/svg'%3E%3Cfilter id='noiseFilter'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='0.8' numOctaves='3' stitchTiles='stitch'/%3E%3C/filter%3E%3Crect width='100%25' height='100%25' filter='url(%23noiseFilter)'/%3E%3C/svg%3E\");\n"
"    }\n"
"    .container {\n"
"      width: 90%;\n"
"      max-width: 420px;\n"
"      padding: 2.5rem 2rem;\n"
"      background: var(--card-bg);\n"
"      border: 1px solid var(--border);\n"
"      border-radius: 20px;\n"
"      box-shadow: 0 10px 30px rgba(217,119,43,0.05);\n"
"      backdrop-filter: blur(10px);\n"
"      text-align: center;\n"
"    }\n"
"    h1 {\n"
"      margin: 0 0 0.5rem 0;\n"
"      font-size: 2rem;\n"
"      font-weight: 700;\n"
"      letter-spacing: -0.03em;\n"
"    }\n"
"    .subtitle {\n"
"      color: var(--text-muted);\n"
"      font-size: 0.9rem;\n"
"      margin-bottom: 2rem;\n"
"      text-transform: uppercase;\n"
"      letter-spacing: 0.1em;\n"
"    }\n"
"    .status-badge {\n"
"      display: inline-block;\n"
"      padding: 0.4rem 1rem;\n"
"      background: rgba(217,119,43,0.1);\n"
"      color: var(--accent);\n"
"      border-radius: 50px;\n"
"      font-size: 0.85rem;\n"
"      font-weight: 600;\n"
"      margin-bottom: 2rem;\n"
"    }\n"
"    .links {\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 0.8rem;\n"
"    }\n"
"    .btn {\n"
"      display: block;\n"
"      padding: 1rem;\n"
"      background: var(--accent);\n"
"      color: #fff;\n"
"      text-decoration: none;\n"
"      border-radius: 12px;\n"
"      font-weight: 600;\n"
"      font-size: 0.95rem;\n"
"      transition: background 0.2s;\n"
"    }\n"
"    .btn:hover {\n"
"      background: #e07227;\n"
"    }\n"
"    .btn-secondary {\n"
"      background: transparent;\n"
"      border: 1px solid var(--border);\n"
"      color: var(--text);\n"
"    }\n"
"    .btn-secondary:hover {\n"
"      background: rgba(217,119,43,0.05);\n"
"    }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class=\"container\">\n"
"    <h1>REFOCUS CAMERA</h1>\n"
"    <div class=\"subtitle\">Development Host</div>\n"
"    <div class=\"status-badge\">CONNECTED // ACTIVE AP</div>\n"
"    <div class=\"links\">\n"
"      <a href=\"/api/handshake\" class=\"btn\">Test Handshake</a>\n"
"      <a href=\"/api/images\" class=\"btn btn-secondary\">Query Image List</a>\n"
"    </div>\n"
"  </div>\n"
"</body>\n"
"</html>";

static esp_err_t get_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t ping_handler(httpd_req_t *req)
{
    add_cors(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
}

/* ─── GET /api/handshake ────────────────────────────────────────────── */


static esp_err_t handshake_handler(httpd_req_t *req)
{
    add_cors(req);
    int total = sdcard_count_images();

    char buf[64];
    int  len = snprintf(buf, sizeof(buf),
                        "{\"status\":\"ok\",\"total\":%d}", total);

    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "Handshake → total=%d", total);
    return httpd_resp_send(req, buf, len);
}

/* ─── GET /api/images ───────────────────────────────────────────────── */
/*
 * Streams a JSON array in chunks so we never have to build the whole list
 * in memory at once.  Each element is:
 *   {"filename":"img_001.jpg","size":12345}
 */
static esp_err_t list_images_handler(httpd_req_t *req)
{
    add_cors(req);
    httpd_resp_set_type(req, "application/json");

    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "%s", SDCARD_IMG_DIR);

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char spool_val[64];
        if (httpd_query_key_value(query, "spool", spool_val, sizeof(spool_val)) == ESP_OK) {
            if (!strchr(spool_val, '/') && !strstr(spool_val, "..")) {
                snprintf(dir_path, sizeof(dir_path), "/sdcard/developed/%s", spool_val);
            }
        }
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        httpd_resp_send_chunk(req, "[", 1);
        httpd_resp_send_chunk(req, "]", 1);
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    /* Chunked transfer — send opening bracket */
    httpd_resp_send_chunk(req, "[", 1);

    bool first = true;
    struct dirent *ent;
    char   chunk[256];
    char   path[512];

    while ((ent = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "Discovered entry: '%s' in path '%s'", ent->d_name, dir_path);

        if (ent->d_type != DT_REG) {
            /* Fallback check in case d_type is not populated by the filesystem */
            char check_path[512];
            snprintf(check_path, sizeof(check_path), "%s/%s", dir_path, ent->d_name);
            struct stat st;
            if (stat(check_path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
        }

        /* Only list files ending in .jpg or .jpeg (case-insensitive) */
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0))
            continue;

        /* Get file size */
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        long fsize = (stat(path, &st) == 0) ? (long)st.st_size : 0;

        int clen = snprintf(chunk, sizeof(chunk),
                            "%s{\"filename\":\"%s\",\"size\":%ld}",
                            first ? "" : ",", ent->d_name, fsize);
        httpd_resp_send_chunk(req, chunk, clen);
        first = false;
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0); /* terminate */
}

static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(char *dst, const char *src, size_t dst_len) {
    size_t written = 0;
    while (*src && (written < dst_len - 1)) {
        if (*src == '%' && is_hex_char(src[1]) && is_hex_char(src[2])) {
            *dst++ = (hex_val(src[1]) << 4) | hex_val(src[2]);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        written++;
    }
    *dst = '\0';
}

/* ─── GET /api/image/<filename> ─────────────────────────────────────── */
/*
 * The URI is registered as "/api/image/[filename]".  We extract the filename from the
 * tail of req->uri.
 */
static esp_err_t get_image_handler(httpd_req_t *req)
{
    add_cors(req);

    /* Extract filename from URI: /api/image/<filename> or /api/images/<filename> */
    const char *prefix_images = "/api/images/";
    const char *prefix_image  = "/api/image/";
    const char *raw_filename  = NULL;

    if (strncmp(req->uri, prefix_images, strlen(prefix_images)) == 0) {
        raw_filename = req->uri + strlen(prefix_images);
    } else if (strncmp(req->uri, prefix_image, strlen(prefix_image)) == 0) {
        raw_filename = req->uri + strlen(prefix_image);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad URI");
        return ESP_FAIL;
    }

    char filename[128];
    url_decode(filename, raw_filename, sizeof(filename));

    /* Truncate query string from filename if present */
    char *query_start = strchr(filename, '?');
    if (query_start) {
        *query_start = '\0';
    }

    /* Sanitise: must not contain '/' or '..' */
    if (strchr(filename, '/') || strstr(filename, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    char path[512];
    char spool_val[64] = {0};
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "spool", spool_val, sizeof(spool_val));
    }

    if (spool_val[0] != '\0' && !strchr(spool_val, '/') && !strstr(spool_val, "..")) {
        snprintf(path, sizeof(path), "/sdcard/developed/%s/%s", spool_val, filename);
    } else {
        snprintf(path, sizeof(path), "%s/%s", SDCARD_IMG_DIR, filename);
    }

    ESP_LOGI(TAG, "Image requested (raw): %s -> (decoded): %s", raw_filename, filename);


    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");

    /* Stream in 16 KB chunks to keep stack usage low and increase throughput */
    const size_t chunk_size = 16384;
    uint8_t *buf = malloc(chunk_size);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    size_t bytes_sent = 0;
    size_t rd;
    int chunk_count = 0;
    while ((rd = fread(buf, 1, chunk_size, f)) > 0) {
        if (httpd_resp_send_chunk(req, (char *)buf, rd) != ESP_OK) {
            ESP_LOGE(TAG, "Send chunk failed for %s", filename);
            ret = ESP_FAIL;
            break;
        }
        bytes_sent += rd;
        
        /* Yield the CPU & shared SDMMC bus once every 4 chunks (every 64 KB)
         * to allow Wi-Fi keepalive/SDIO exchanges to process without inserting unnecessary latency */
        if (++chunk_count % 4 == 0) {
            vTaskDelay(1);
        }
    }
    fclose(f);
    free(buf);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sent %s (%zu bytes)", filename, bytes_sent);
        httpd_resp_send_chunk(req, NULL, 0); /* terminate */
    }
    return ret;
}

/* ─── GET /api/spools ──────────────────────────────────────────────── */
static esp_err_t list_spools_handler(httpd_req_t *req)
{
    add_cors(req);
    httpd_resp_set_type(req, "application/json");

    DIR *dir = opendir("/sdcard/developed");
    if (!dir) {
        return httpd_resp_send(req, "[]", 2);
    }

    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    struct dirent *ent;
    char chunk[256];

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "spool_", 6) == 0) {
            char path[300];
            snprintf(path, sizeof(path), "/sdcard/developed/%s", ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                int clen = snprintf(chunk, sizeof(chunk),
                                    "%s\"%s\"",
                                    first ? "" : ",", ent->d_name);
                httpd_resp_send_chunk(req, chunk, clen);
                first = false;
            }
        }
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ─── GET /api/spool/next ──────────────────────────────────────────── */
static esp_err_t get_next_spool_handler(httpd_req_t *req)
{
    add_cors(req);
    int next_spool_idx = sdcard_get_next_spool_index();
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "{\"next_spool\":\"spool_%d\"}", next_spool_idx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ─── POST /api/spool/wipe ─────────────────────────────────────────── */
static esp_err_t wipe_spool_handler(httpd_req_t *req)
{
    add_cors(req);

    char content[128] = {0};
    int ret_len = httpd_req_recv(req, content, sizeof(content) - 1);
    char requested_name[64] = {0};

    if (ret_len > 0) {
        content[ret_len] = '\0';
        cJSON *root = cJSON_Parse(content);
        if (root) {
            cJSON *name_item = cJSON_GetObjectItem(root, "name");
            if (name_item && cJSON_IsString(name_item)) {
                snprintf(requested_name, sizeof(requested_name), "%s", name_item->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    esp_err_t ret = sdcard_wipe_spool(requested_name[0] ? requested_name : NULL);
    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wipe failed");
        return ESP_FAIL;
    }
}

/* ─── POST /api/spool/delete ───────────────────────────────────────── */
static esp_err_t delete_spool_handler(httpd_req_t *req)
{
    add_cors(req);

    char content[128] = {0};
    int ret_len = httpd_req_recv(req, content, sizeof(content) - 1);
    char requested_name[64] = {0};

    if (ret_len > 0) {
        content[ret_len] = '\0';
        cJSON *root = cJSON_Parse(content);
        if (root) {
            cJSON *name_item = cJSON_GetObjectItem(root, "name");
            if (name_item && cJSON_IsString(name_item)) {
                snprintf(requested_name, sizeof(requested_name), "%s", name_item->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    if (!requested_name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing spool name");
        return ESP_FAIL;
    }

    esp_err_t ret = sdcard_delete_spool(requested_name);
    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }
}

/* ─── POST /api/spool/rename ───────────────────────────────────────── */
static esp_err_t rename_spool_handler(httpd_req_t *req)
{
    add_cors(req);

    char content[256] = {0};
    int ret_len = httpd_req_recv(req, content, sizeof(content) - 1);
    char old_name[64] = {0};
    char new_name[64] = {0};

    if (ret_len > 0) {
        content[ret_len] = '\0';
        cJSON *root = cJSON_Parse(content);
        if (root) {
            cJSON *old_item = cJSON_GetObjectItem(root, "old_name");
            cJSON *new_item = cJSON_GetObjectItem(root, "new_name");
            if (old_item && cJSON_IsString(old_item)) {
                snprintf(old_name, sizeof(old_name), "%s", old_item->valuestring);
            }
            if (new_item && cJSON_IsString(new_item)) {
                snprintf(new_name, sizeof(new_name), "%s", new_item->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    if (!old_name[0] || !new_name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing old_name or new_name");
        return ESP_FAIL;
    }

    esp_err_t ret = sdcard_rename_spool(old_name, new_name);
    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }
}



/* ─── server lifecycle ──────────────────────────────────────────────── */
esp_err_t webserver_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;
    cfg.max_uri_handlers = 14;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    /* OPTIONS preflight — catch-all */
    static const httpd_uri_t options_uri = {
        .uri     = "/*",
        .method  = HTTP_OPTIONS,
        .handler = options_handler,
    };
    httpd_register_uri_handler(s_server, &options_uri);

    /* GET / */
    static const httpd_uri_t index_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_index_handler,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    /* GET /api/handshake */
    static const httpd_uri_t handshake_uri = {
        .uri     = "/api/handshake",
        .method  = HTTP_GET,
        .handler = handshake_handler,
    };
    httpd_register_uri_handler(s_server, &handshake_uri);

    /* GET /api/ping */
    static const httpd_uri_t ping_uri = {
        .uri     = "/api/ping",
        .method  = HTTP_GET,
        .handler = ping_handler,
    };
    httpd_register_uri_handler(s_server, &ping_uri);

    /* GET /api/images */
    static const httpd_uri_t images_uri = {
        .uri     = "/api/images",
        .method  = HTTP_GET,
        .handler = list_images_handler,
    };
    httpd_register_uri_handler(s_server, &images_uri);

    /* GET /api/image/[filename] — individual file download */
    static const httpd_uri_t image_uri = {
        .uri     = "/api/image/*",
        .method  = HTTP_GET,
        .handler = get_image_handler,
    };
    httpd_register_uri_handler(s_server, &image_uri);

    /* GET /api/images/[filename] — individual file download (plural prefix support) */
    static const httpd_uri_t images_file_uri = {
        .uri     = "/api/images/*",
        .method  = HTTP_GET,
        .handler = get_image_handler,
    };
    httpd_register_uri_handler(s_server, &images_file_uri);

    /* GET /api/spools — retrieve the list of developed spool folder names */
    static const httpd_uri_t list_spools_uri = {
        .uri     = "/api/spools",
        .method  = HTTP_GET,
        .handler = list_spools_handler,
    };
    httpd_register_uri_handler(s_server, &list_spools_uri);

    /* GET /api/spool/next — retrieve the next default spool folder name */
    static const httpd_uri_t get_next_spool_uri = {
        .uri     = "/api/spool/next",
        .method  = HTTP_GET,
        .handler = get_next_spool_handler,
    };
    httpd_register_uri_handler(s_server, &get_next_spool_uri);

    /* POST /api/spool/wipe — wipe the active spool and archive files */
    static const httpd_uri_t wipe_spool_uri = {
        .uri     = "/api/spool/wipe",
        .method  = HTTP_POST,
        .handler = wipe_spool_handler,
    };
    httpd_register_uri_handler(s_server, &wipe_spool_uri);

    /* POST /api/spool/delete — delete an archived spool folder and its contents */
    static const httpd_uri_t delete_spool_uri = {
        .uri     = "/api/spool/delete",
        .method  = HTTP_POST,
        .handler = delete_spool_handler,
    };
    httpd_register_uri_handler(s_server, &delete_spool_uri);

    /* POST /api/spool/rename — rename an archived spool folder */
    static const httpd_uri_t rename_spool_uri = {
        .uri     = "/api/spool/rename",
        .method  = HTTP_POST,
        .handler = rename_spool_handler,
    };
    httpd_register_uri_handler(s_server, &rename_spool_uri);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

void webserver_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
