#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "app_config.h"
#include "web_server.h"
#include "net_manager.h"
#include "rp_manager.h"
#include "ilda_player.h"
#include "ilda_preview.h"
#include "sd_manager.h"
#include "protocol_stream_stats.h"
#include "protocol_settings.h"
#include "idn_protocol.h"
#include "etherdream_protocol.h"
#include "ilw_protocol.h"

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;
static TaskHandle_t s_refresh_task = NULL;
static QueueHandle_t s_preview_queue = NULL;

#define WEB_SERVER_TASK_STACK_BYTES 16384
#define WEB_SD_PATH_MAX 191
#define WEB_SD_REL_PATH_MAX (WEB_SD_PATH_MAX - sizeof(SD_MOUNT_POINT))
#define WEB_RP_STATUS_JSON_BYTES 2560
#define WEB_NETWORK_JSON_BYTES 2048
#define WEB_PROTOCOL_JSON_BYTES 960
#define WEB_JSON_CHUNK_BYTES 768
#define WEB_PREVIEW_TASK_STACK_BYTES WEB_SERVER_TASK_STACK_BYTES
#define WEB_PREVIEW_QUEUE_LENGTH 2

/* ===================== BACKGROUND DAC ENGINE REFRESH ===================== */

static void dac_engine_refresh_task(void *arg)
{
    (void)arg;

    while (1) {
        rp_manager_ping();
        rp_manager_get_remote_status();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================== URL DECODE ===================== */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);

            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }

        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

/* ===================== HELPERS ===================== */

static void set_common_response_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t send_plain_result(httpd_req_t *req, esp_err_t ret)
{
    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");

    if (ret == ESP_OK) {
        return httpd_resp_sendstr(req, "OK");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "ERR %s", esp_err_to_name(ret));
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t send_bad_request(httpd_req_t *req, const char *msg)
{
    set_common_response_headers(req);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");

    char buf[160];
    snprintf(buf, sizeof(buf), "ERR %s", msg ? msg : "Bad request");
    return httpd_resp_sendstr(req, buf);
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t off = 0;

    if (!src) {
        src = "";
    }

    for (size_t i = 0; src[i] && off + 1 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];

        if ((ch == '"' || ch == '\\') && off + 2 < dst_len) {
            dst[off++] = '\\';
            dst[off++] = (char)ch;
        } else if (ch >= 0x20) {
            dst[off++] = (char)ch;
        }
    }

    dst[off] = '\0';
}

static void copy_bounded_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    size_t len = strnlen(src, dst_len - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    if (!body || !key || !out || out_len == 0) {
        return false;
    }

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);

        if (eq) {
            size_t this_key_len = (size_t)(eq - p);

            if (this_key_len == key_len && strncmp(p, key, key_len) == 0) {
                size_t value_len = pair_len - this_key_len - 1;
                if (value_len >= out_len) {
                    value_len = out_len - 1;
                }

                memcpy(out, eq + 1, value_len);
                out[value_len] = '\0';
                url_decode_inplace(out);
                return true;
            }
        }

        if (!amp) {
            break;
        }

        p = amp + 1;
    }

    return false;
}

static bool form_value_enabled(const char *value)
{
    return strcmp(value, "1") == 0 ||
           strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "enabled") == 0 ||
           strcasecmp(value, "on") == 0;
}

static esp_err_t read_request_body(httpd_req_t *req, char *body, size_t body_len)
{
    int total = req->content_len;

    if (total < 0 || (size_t)total >= body_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);

        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (ret <= 0) {
            return ESP_FAIL;
        }

        received += ret;
    }

    body[received] = '\0';
    return ESP_OK;
}

/* ===================== CHUNKED JSON ===================== */

typedef struct {
    httpd_req_t *req;
    char chunk[WEB_JSON_CHUNK_BYTES];
    size_t chunk_len;
} json_chunk_stream_t;

static esp_err_t json_stream_append(json_chunk_stream_t *stream,
                                    const char *data, size_t data_len)
{
    while (data_len > 0) {
        if (stream->chunk_len == sizeof(stream->chunk)) {
            esp_err_t ret = httpd_resp_send_chunk(stream->req, stream->chunk,
                                                  stream->chunk_len);
            if (ret != ESP_OK) {
                return ret;
            }

            stream->chunk_len = 0;
        }

        size_t available = sizeof(stream->chunk) - stream->chunk_len;
        size_t copy_len = data_len < available ? data_len : available;

        memcpy(stream->chunk + stream->chunk_len, data, copy_len);
        stream->chunk_len += copy_len;
        data += copy_len;
        data_len -= copy_len;
    }

    return ESP_OK;
}

static esp_err_t json_stream_append_formatted(json_chunk_stream_t *stream,
                                              char *buf, size_t buf_len, int len)
{
    if (len < 0 || (size_t)len >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return json_stream_append(stream, buf, (size_t)len);
}

static esp_err_t json_stream_finish(json_chunk_stream_t *stream)
{
    if (stream->chunk_len > 0) {
        esp_err_t ret = httpd_resp_send_chunk(stream->req, stream->chunk,
                                              stream->chunk_len);
        if (ret != ESP_OK) {
            return ret;
        }

        stream->chunk_len = 0;
    }

    return httpd_resp_send_chunk(stream->req, NULL, 0);
}

/* ===================== SD LIST ===================== */

static bool valid_sd_relative_path(const char *path)
{
    if (!path || path[0] == '/' || path[0] == '\\' ||
        strlen(path) > WEB_SD_REL_PATH_MAX) {
        return false;
    }

    if (path[0] == '\0') {
        return true;
    }

    const char *segment = path;

    for (const char *cursor = path;; cursor++) {
        unsigned char ch = (unsigned char)*cursor;

        if (ch == '\\' || (ch != '\0' && ch < 0x20)) {
            return false;
        }

        if (ch == '/' || ch == '\0') {
            size_t segment_len = (size_t)(cursor - segment);

            if (segment_len == 0 ||
                (segment_len == 1 && segment[0] == '.') ||
                (segment_len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }

            if (ch == '\0') {
                break;
            }

            segment = cursor + 1;
        }
    }

    return true;
}

static esp_err_t sd_list_get_handler(httpd_req_t *req)
{
    (void)sd_manager_rescan();

    sd_status_t sd = {0};
    sd_manager_get_status(&sd);

    char query[WEB_SD_REL_PATH_MAX * 3 + 16] = {0};
    char encoded_path[WEB_SD_REL_PATH_MAX * 3 + 1] = {0};
    char current_path[WEB_SD_REL_PATH_MAX + 1] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        esp_err_t query_ret = httpd_query_key_value(query, "path", encoded_path, sizeof(encoded_path));

        if (query_ret == ESP_OK) {
            url_decode_inplace(encoded_path);
            if (!valid_sd_relative_path(encoded_path)) {
                return send_bad_request(req, "Invalid SD directory path");
            }

            strlcpy(current_path, encoded_path, sizeof(current_path));
        } else if (strstr(query, "path=") != NULL) {
            return send_bad_request(req, "SD directory path is too long");
        }
    }

    if (!sd.mounted) {
        set_common_response_headers(req);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"mounted\":false,\"current\":\"\",\"count\":0,\"files\":[]}");
    }

    char directory_path[WEB_SD_PATH_MAX + 1];
    int directory_path_len = current_path[0]
        ? snprintf(directory_path, sizeof(directory_path), "%s/%s", SD_MOUNT_POINT, current_path)
        : snprintf(directory_path, sizeof(directory_path), "%s", SD_MOUNT_POINT);

    if (directory_path_len < 0 || (size_t)directory_path_len >= sizeof(directory_path)) {
        return send_bad_request(req, "SD directory path is too long");
    }

    struct stat directory_stat;
    if (stat(directory_path, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode)) {
        set_common_response_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"Directory not found\"}");
    }

    DIR *dir = opendir(directory_path);

    if (!dir) {
        set_common_response_headers(req);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"mounted\":false,\"current\":\"\",\"count\":0,\"files\":[]}");
    }

    json_chunk_stream_t *stream = malloc(sizeof(*stream));
    if (!stream) {
        closedir(dir);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    stream->req = req;
    stream->chunk_len = 0;

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");

    /*
       Stream entries in chunks so large directories are listed in full
       instead of being cut off at a fixed response buffer size.
    */
    char item[1200];
    char escaped_current[WEB_SD_REL_PATH_MAX * 2 + 1];
    json_escape(current_path, escaped_current, sizeof(escaped_current));

    esp_err_t ret = json_stream_append_formatted(stream, item, sizeof(item),
        snprintf(item, sizeof(item), "{\"mounted\":true,\"current\":\"%s\",\"files\":[",
                 escaped_current));

    int count = 0;
    struct dirent *ent;

    while (ret == ESP_OK && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        char path[WEB_SD_PATH_MAX + 1];
        int path_len = snprintf(path, sizeof(path), "%s/%s", directory_path, ent->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            continue;
        }

        char browse_path[WEB_SD_REL_PATH_MAX + 1];
        int browse_path_len = current_path[0]
            ? snprintf(browse_path, sizeof(browse_path), "%s/%s", current_path, ent->d_name)
            : snprintf(browse_path, sizeof(browse_path), "%s", ent->d_name);
        if (browse_path_len < 0 || (size_t)browse_path_len >= sizeof(browse_path)) {
            continue;
        }

        struct stat st;
        memset(&st, 0, sizeof(st));

        if (stat(path, &st) != 0) {
            continue;
        }

        bool is_dir = S_ISDIR(st.st_mode);
        const char *type = is_dir ? "directory" : "file";
        const char *dot = strrchr(ent->d_name, '.');

        if (!is_dir && dot) {
            if (strcasecmp(dot, ".ild") == 0) {
                type = "ILDA";
            } else if (strcasecmp(dot, ".txt") == 0) {
                type = "Text";
            } else {
                type = dot + 1;
            }
        }

        char escaped_name[512];
        char escaped_path[WEB_SD_PATH_MAX * 2 + 1];
        char escaped_browse_path[WEB_SD_REL_PATH_MAX * 2 + 1];
        char escaped_type[64];

        json_escape(ent->d_name, escaped_name, sizeof(escaped_name));
        json_escape(path, escaped_path, sizeof(escaped_path));
        json_escape(browse_path, escaped_browse_path, sizeof(escaped_browse_path));
        json_escape(type, escaped_type, sizeof(escaped_type));

        int item_len = snprintf(item, sizeof(item),
                                "%s{\"name\":\"%s\",\"path\":\"%s\",\"browse_path\":\"%s\",\"size\":%lu,\"type\":\"%s\",\"is_dir\":%s}",
                                count > 0 ? "," : "",
                                escaped_name, escaped_path, escaped_browse_path,
                                (unsigned long)st.st_size, escaped_type,
                                is_dir ? "true" : "false");
        if (item_len < 0 || (size_t)item_len >= sizeof(item)) {
            continue;
        }

        ret = json_stream_append(stream, item, (size_t)item_len);
        count++;
    }

    closedir(dir);

    if (ret == ESP_OK) {
        ret = json_stream_append_formatted(stream, item, sizeof(item),
            snprintf(item, sizeof(item), "],\"count\":%d}", count));
    }

    if (ret == ESP_OK) {
        ret = json_stream_finish(stream);
    }

    free(stream);
    return ret;
}

/* ===================== ILDA PREVIEW ===================== */

static esp_err_t preview_stream_points(json_chunk_stream_t *stream,
                                       const ilda_preview_point_t *points,
                                       uint32_t point_count)
{
    for (uint32_t i = 0; i < point_count; i++) {
        const ilda_preview_point_t *point = &points[i];
        char item[64];

        esp_err_t ret = json_stream_append_formatted(stream, item, sizeof(item),
            snprintf(item, sizeof(item), "%s[%d,%d,%u,%u,%u,%u]",
                     i == 0 ? "" : ",", point->x, point->y,
                     point->r, point->g, point->b,
                     point->blanked ? 1U : 0U));
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t preview_frame_stream_cb(void *context, uint32_t frame_index,
                                         uint16_t frame_points,
                                         const ilda_preview_point_t *points,
                                         uint32_t point_count)
{
    json_chunk_stream_t *stream = context;
    char header[48];

    esp_err_t ret = json_stream_append_formatted(stream, header, sizeof(header),
        snprintf(header, sizeof(header), "%s{\"point_count\":%u,\"points\":[",
                 frame_index > 0 ? "," : "", (unsigned)frame_points));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = preview_stream_points(stream, points, point_count);
    if (ret != ESP_OK) {
        return ret;
    }

    return json_stream_append(stream, "]}", 2);
}

static esp_err_t ilda_preview_send(httpd_req_t *req)
{
    char query[WEB_SD_REL_PATH_MAX * 3 + 32] = {0};
    char encoded_path[WEB_SD_REL_PATH_MAX * 3 + 1] = {0};
    char relative_path[WEB_SD_REL_PATH_MAX + 1] = {0};
    char frame_value[16] = {0};
    char all_frames_value[8] = {0};
    char full_path[WEB_SD_PATH_MAX + 1];
    uint32_t selected_frame = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", encoded_path, sizeof(encoded_path)) != ESP_OK) {
        return send_bad_request(req, "An ILDA file path is required");
    }

    bool all_frames = httpd_query_key_value(query, "all", all_frames_value,
                                            sizeof(all_frames_value)) == ESP_OK &&
                      strcmp(all_frames_value, "1") == 0;

    url_decode_inplace(encoded_path);
    if (!encoded_path[0] || !valid_sd_relative_path(encoded_path)) {
        return send_bad_request(req, "Invalid SD file path");
    }

    strlcpy(relative_path, encoded_path, sizeof(relative_path));

    const char *extension = strrchr(relative_path, '.');
    if (!extension || strcasecmp(extension, ".ild") != 0) {
        return send_bad_request(req, "Only ILDA files can be previewed");
    }

    if (httpd_query_key_value(query, "frame", frame_value, sizeof(frame_value)) == ESP_OK) {
        char *end = NULL;
        unsigned long parsed = strtoul(frame_value, &end, 10);

        if (frame_value[0] == '\0' || !end || *end != '\0' || parsed > UINT32_MAX) {
            return send_bad_request(req, "Invalid frame index");
        }

        selected_frame = (uint32_t)parsed;
    }

    sd_status_t sd = {0};
    sd_manager_get_status(&sd);
    if (!sd.mounted) {
        return send_bad_request(req, "SD card is not mounted");
    }

    int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, relative_path);
    if (path_len < 0 || (size_t)path_len >= sizeof(full_path)) {
        return send_bad_request(req, "SD file path is too long");
    }

    struct stat file_stat;
    if (stat(full_path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
        return send_bad_request(req, "ILDA file not found");
    }

    ilda_preview_result_t *preview = calloc(1, sizeof(*preview));
    json_chunk_stream_t *stream = malloc(sizeof(*stream));
    if (!preview || !stream) {
        free(preview);
        free(stream);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    stream->req = req;
    stream->chunk_len = 0;

    esp_err_t ret = ilda_preview_analyze(full_path, selected_frame,
                                         (uint64_t)file_stat.st_size, preview);
    if (ret != ESP_OK) {
        free(preview);
        free(stream);

        if (ret == ESP_ERR_NOT_SUPPORTED) {
            return send_bad_request(req, "ILDA format is not supported by the player");
        }

        if (ret == ESP_ERR_NOT_FOUND) {
            return send_bad_request(req, "Frame index is outside this file");
        }

        return send_bad_request(req, "Unable to parse ILDA file");
    }

    if (all_frames && preview->frame_count > ILDA_PREVIEW_MAX_ANIMATED_FRAMES) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Animated preview supports up to %u frames",
                 (unsigned)ILDA_PREVIEW_MAX_ANIMATED_FRAMES);
        free(preview);
        free(stream);
        return send_bad_request(req, msg);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");

    char prefix[384];
    ret = json_stream_append_formatted(stream, prefix, sizeof(prefix),
        snprintf(prefix, sizeof(prefix),
                 "{\"file_size\":%llu,\"frame_count\":%lu,\"total_points\":%lu,"
                 "\"max_frame_points\":%lu,\"selected_frame\":%lu,\"selected_frame_points\":%lu,"
                 "\"format_mask\":%lu,\"max_samples\":%lu,\"within_sample_limit\":%s,"
                 "\"loop_return_samples\":%u,%s",
                 (unsigned long long)file_stat.st_size,
                 (unsigned long)preview->frame_count,
                 (unsigned long)preview->total_points,
                 (unsigned long)preview->max_frame_points,
                 (unsigned long)preview->selected_frame,
                 (unsigned long)preview->selected_frame_points,
                 (unsigned long)preview->format_mask,
                 (unsigned long)preview->max_samples,
                 preview->within_sample_limit ? "true" : "false",
                 (unsigned)ILDA_LOOP_RETURN_BLANK_SAMPLES,
                 all_frames ? "\"frames\":[" : "\"points\":["));

    if (ret == ESP_OK) {
        if (all_frames) {
            ret = ilda_preview_stream_frames(full_path, (uint64_t)file_stat.st_size,
                                             preview_frame_stream_cb, stream, preview);
        } else {
            ret = preview_stream_points(stream, preview->points, preview->preview_point_count);
        }
    }

    if (ret == ESP_OK) {
        ret = json_stream_append(stream, "]}", 2);
    }

    if (ret == ESP_OK) {
        ret = json_stream_finish(stream);
    }

    free(preview);
    free(stream);
    return ret;
}

static void ilda_preview_task(void *arg)
{
    (void)arg;

    while (1) {
        httpd_req_t *req = NULL;

        if (xQueueReceive(s_preview_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        (void)ilda_preview_send(req);
        httpd_req_async_handler_complete(req);
    }
}

static esp_err_t ilda_preview_get_handler(httpd_req_t *req)
{
    /*
       A preview can stream megabytes from the SD card. Hand it to a worker
       task so the server task stays free for ILDA stop and status requests.
    */
    httpd_req_t *async_req = NULL;

    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Preview unavailable");
    }

    if (xQueueSend(s_preview_queue, &async_req, 0) != pdTRUE) {
        set_common_response_headers(async_req);
        httpd_resp_set_status(async_req, "503 Service Unavailable");
        httpd_resp_set_type(async_req, "text/plain");
        httpd_resp_sendstr(async_req, "ERR Preview busy, try again");
        httpd_req_async_handler_complete(async_req);
    }

    return ESP_OK;
}

/* ===================== STATUS ===================== */

static esp_err_t rp_status_get_handler(httpd_req_t *req)
{
    rp_status_t rp;
    rp_manager_get_status(&rp);

    sd_status_t sd = {0};
    sd_manager_get_status(&sd);

    protocol_stream_stats_t streams = {0};
    protocol_stream_stats_get(&streams);

    char *json = malloc(WEB_RP_STATUS_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    /* Report the playing file relative to the card root, as the SD list does. */
    char ilda_file[ILDA_PATH_MAX];
    bool ilda_folder = false;
    ilda_player_get_current(ilda_file, sizeof(ilda_file), &ilda_folder);

    const char *ilda_relative = ilda_file;
    if (strncmp(ilda_file, SD_MOUNT_POINT "/", sizeof(SD_MOUNT_POINT)) == 0) {
        ilda_relative = ilda_file + sizeof(SD_MOUNT_POINT);
    }

    char escaped_ilda_file[ILDA_PATH_MAX * 2];
    json_escape(ilda_relative, escaped_ilda_file, sizeof(escaped_ilda_file));

    snprintf(json, WEB_RP_STATUS_JSON_BYTES,
        "{"
            "\"product\":\"%s\","
            "\"esp_fw\":\"%s\","
            "\"rp_fw\":\"%s\","
            "\"initialized\":%s,"
            "\"remote_running\":%s,"
            "\"remote_sample_rate\":%lu,"
            "\"remote_payload_errors\":%lu,"
            "\"remote_buffer_fill_samples\":%lu,"
            "\"remote_buffer_size_samples\":%lu,"
            "\"remote_buffer_high_water_samples\":%lu,"
            "\"remote_buffer_low_water_samples\":%lu,"
            "\"remote_buffer_max_samples\":%lu,"
            "\"remote_mirror_x\":%s,"
            "\"remote_mirror_y\":%s,"
            "\"rate_limit\":%lu,"
            "\"remote_max_rate\":%lu,"
            "\"remote_requested_rate\":%lu,"
            "\"remote_max_lit_step\":%lu,"
            "\"remote_lit_big_steps\":%lu,"
            "\"remote_blank_big_steps\":%lu,"
            "\"remote_color_shift_us\":%ld,"
            "\"remote_color_shift_samples\":%lu,"
            "\"remote_color_shift_saved\":%s,"
            "\"remote_stale_packets\":%lu,"
            "\"remote_underruns\":%lu,"
            "\"remote_stream_packets\":%lu,"
            "\"remote_stream_bad_packets\":%lu,"
            "\"remote_stream_dropped_packets\":%lu,"
            "\"remote_stream_rx_timeouts\":%lu,"
            "\"remote_seq_gaps\":%lu,"
            "\"remote_seq_missing\":%lu,"
            "\"remote_rx_partial\":%lu,"
            "\"payload_ready_timeouts\":%lu,"
            "\"payload_spi_errors\":%lu,"
            "\"payload_ack_timeouts\":%lu,"
            "\"sd_inserted\":%s,"
            "\"sd_mounted\":%s,"
            "\"sd_file_count\":%d,"
            "\"stream_sd_ilda_samples\":%lu,"
            "\"stream_idn_samples\":%lu,"
            "\"stream_etherdream_samples\":%lu,"
            "\"stream_ilw_samples\":%lu,"
            "\"stream_sd_ilda_dropped\":%lu,"
            "\"stream_idn_dropped\":%lu,"
            "\"stream_etherdream_dropped\":%lu,"
            "\"stream_ilw_dropped\":%lu,"
            "\"test_stream_running\":%s,"
            "\"ilda_running\":%s,"
            "\"ilda_file\":\"%s\","
            "\"ilda_folder\":%s"
        "}",
        PRODUCT_NAME,
        FW_VERSION,
        rp.remote_fw_version[0] ? rp.remote_fw_version : "unknown",
        rp.initialized ? "true" : "false",
        rp.remote_running ? "true" : "false",
        (unsigned long)rp.remote_sample_rate,
        (unsigned long)rp.remote_payload_errors,
        (unsigned long)rp.remote_buffer_fill_samples,
        (unsigned long)rp.remote_buffer_size_samples,
        (unsigned long)rp.remote_buffer_high_water_samples,
        (unsigned long)rp.remote_buffer_low_water_samples,
        (unsigned long)rp.remote_buffer_max_samples,
        rp.remote_mirror_x ? "true" : "false",
        rp.remote_mirror_y ? "true" : "false",
        (unsigned long)rp.rate_limit,
        (unsigned long)rp.remote_max_rate,
        (unsigned long)rp.remote_requested_rate,
        (unsigned long)rp.remote_max_lit_step,
        (unsigned long)rp.remote_lit_big_steps,
        (unsigned long)rp.remote_blank_big_steps,
        (long)rp.remote_color_shift_us,
        (unsigned long)rp.remote_color_shift_samples,
        rp.remote_color_shift_saved ? "true" : "false",
        (unsigned long)rp.remote_stale_packets,
        (unsigned long)rp.remote_underruns,
        (unsigned long)rp.remote_stream_packets,
        (unsigned long)rp.remote_stream_bad_packets,
        (unsigned long)rp.remote_stream_dropped_packets,
        (unsigned long)rp.remote_stream_rx_timeouts,
        (unsigned long)rp.remote_seq_gaps,
        (unsigned long)rp.remote_seq_missing,
        (unsigned long)rp.remote_rx_partial,
        (unsigned long)rp.payload_ready_timeouts,
        (unsigned long)rp.payload_spi_errors,
        (unsigned long)rp.payload_ack_timeouts,
        sd.inserted ? "true" : "false",
        sd.mounted ? "true" : "false",
        sd.file_count,
        (unsigned long)streams.sd_ilda_samples,
        (unsigned long)streams.idn_samples,
        (unsigned long)streams.etherdream_samples,
        (unsigned long)streams.ilw_samples,
        (unsigned long)streams.sd_ilda_dropped,
        (unsigned long)streams.idn_dropped,
        (unsigned long)streams.etherdream_dropped,
        (unsigned long)streams.ilw_dropped,
        rp.test_stream_running ? "true" : "false",
        ilda_player_is_running() ? "true" : "false",
        escaped_ilda_file,
        ilda_folder ? "true" : "false"
    );

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

/* ===================== NETWORK SETTINGS ===================== */

static esp_err_t network_config_get_handler(httpd_req_t *req)
{
    net_settings_t cfg;
    net_status_t status;

    net_manager_get_settings(&cfg);
    net_manager_get_status(&status);

    char ap_ssid[96];
    char sta_ssid[96];
    char hostname[96];
    char ip[32];
    char mask[32];
    char gw[32];
    char dns[32];

    json_escape(cfg.wifi_ap_ssid, ap_ssid, sizeof(ap_ssid));
    json_escape(cfg.wifi_sta_ssid, sta_ssid, sizeof(sta_ssid));
    json_escape(cfg.hostname, hostname, sizeof(hostname));
    json_escape(cfg.static_ip, ip, sizeof(ip));
    json_escape(cfg.static_netmask, mask, sizeof(mask));
    json_escape(cfg.static_gateway, gw, sizeof(gw));
    json_escape(cfg.static_dns, dns, sizeof(dns));

    char *json = malloc(WEB_NETWORK_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    snprintf(json, WEB_NETWORK_JSON_BYTES,
        "{"
            "\"wifi_ap_enabled\":%s,"
            "\"wifi_ap_ssid\":\"%s\","
            "\"wifi_ap_password_set\":%s,"
            "\"wifi_sta_enabled\":%s,"
            "\"wifi_sta_ssid\":\"%s\","
            "\"wifi_sta_password_set\":%s,"
            "\"hostname\":\"%s\","
            "\"ip_mode\":\"%s\","
            "\"static_ip\":\"%s\","
            "\"static_netmask\":\"%s\","
            "\"static_gateway\":\"%s\","
            "\"static_dns\":\"%s\","
            "\"reboot_required\":%s,"
            "\"status\":{"
                "\"eth_link\":%s,"
                "\"eth_has_ip\":%s,"
                "\"eth_ip\":\"%s\","
                "\"wifi_ap_enabled\":%s,"
                "\"wifi_ap_ip\":\"%s\","
                "\"wifi_sta_enabled\":%s,"
                "\"wifi_sta_connected\":%s,"
                "\"wifi_sta_ip\":\"%s\""
            "}"
        "}",
        cfg.wifi_ap_enabled ? "true" : "false",
        ap_ssid,
        cfg.wifi_ap_password[0] ? "true" : "false",
        cfg.wifi_sta_enabled ? "true" : "false",
        sta_ssid,
        cfg.wifi_sta_password[0] ? "true" : "false",
        hostname,
        cfg.ip_mode == NET_IP_MODE_STATIC ? "static" : "dhcp",
        ip,
        mask,
        gw,
        dns,
        net_manager_settings_pending_reboot() ? "true" : "false",
        status.eth_link ? "true" : "false",
        status.eth_has_ip ? "true" : "false",
        status.eth_ip,
        status.wifi_ap_enabled ? "true" : "false",
        status.wifi_ap_ip,
        status.wifi_sta_enabled ? "true" : "false",
        status.wifi_sta_connected ? "true" : "false",
        status.wifi_sta_ip
    );

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

static esp_err_t network_config_post_handler(httpd_req_t *req)
{
    char body[1024];
    esp_err_t ret = read_request_body(req, body, sizeof(body));

    if (ret != ESP_OK) {
        return send_bad_request(req, "Network settings form is too large");
    }

    net_settings_t cfg;
    net_manager_get_settings(&cfg);

    char value[128];

    if (form_get_value(body, "ap_enabled", value, sizeof(value))) {
        cfg.wifi_ap_enabled = form_value_enabled(value);
    }

    if (form_get_value(body, "ap_ssid", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_ap_ssid, sizeof(cfg.wifi_ap_ssid), value);
    }

    if (form_get_value(body, "ap_password", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_ap_password, sizeof(cfg.wifi_ap_password), value);
    }

    if (form_get_value(body, "sta_enabled", value, sizeof(value))) {
        cfg.wifi_sta_enabled = form_value_enabled(value);
    }

    if (form_get_value(body, "sta_ssid", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_sta_ssid, sizeof(cfg.wifi_sta_ssid), value);
    }

    if (form_get_value(body, "sta_password", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_sta_password, sizeof(cfg.wifi_sta_password), value);
    }

    if (form_get_value(body, "hostname", value, sizeof(value))) {
        copy_bounded_string(cfg.hostname, sizeof(cfg.hostname), value);
    }

    if (form_get_value(body, "ip_mode", value, sizeof(value))) {
        cfg.ip_mode = strcmp(value, "static") == 0 ? NET_IP_MODE_STATIC : NET_IP_MODE_DHCP;
    }

    if (form_get_value(body, "static_ip", value, sizeof(value))) {
        copy_bounded_string(cfg.static_ip, sizeof(cfg.static_ip), value);
    }

    if (form_get_value(body, "static_netmask", value, sizeof(value))) {
        copy_bounded_string(cfg.static_netmask, sizeof(cfg.static_netmask), value);
    }

    if (form_get_value(body, "static_gateway", value, sizeof(value))) {
        copy_bounded_string(cfg.static_gateway, sizeof(cfg.static_gateway), value);
    }

    if (form_get_value(body, "static_dns", value, sizeof(value))) {
        copy_bounded_string(cfg.static_dns, sizeof(cfg.static_dns), value);
    }

    char err[160];
    ret = net_manager_validate_settings(&cfg, err, sizeof(err));
    if (ret != ESP_OK) {
        return send_bad_request(req, err);
    }

    ret = net_manager_save_settings(&cfg);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    char json[64];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"reboot_required\":%s}",
             net_manager_settings_pending_reboot() ? "true" : "false");

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* ===================== PROTOCOL SETTINGS ===================== */

static esp_err_t protocol_config_get_handler(httpd_req_t *req)
{
    protocol_settings_t cfg;
    protocol_settings_t active;

    protocol_settings_get(&cfg);
    protocol_settings_get_active(&active);

    char *json = malloc(WEB_PROTOCOL_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    snprintf(json, WEB_PROTOCOL_JSON_BYTES,
        "{"
            "\"sd_ilda_enabled\":true,"
            "\"sd_ilda_locked\":true,"
            "\"sd_ilda_transport\":\"Local SD\","
            "\"sd_ilda_ports\":\"None\","
            "\"idn_enabled\":%s,"
            "\"idn_transport\":\"UDP\","
            "\"idn_udp_port\":%d,"
            "\"etherdream_enabled\":%s,"
            "\"etherdream_transport\":\"TCP + UDP\","
            "\"etherdream_tcp_port\":%d,"
            "\"etherdream_udp_port\":%d,"
            "\"ilw_enabled\":%s,"
            "\"ilw_transport\":\"UDP\","
            "\"ilw_udp_port\":%d,"
            "\"reboot_required\":%s,"
            "\"active\":{"
                "\"sd_ilda_enabled\":true,"
                "\"idn_enabled\":%s,"
                "\"etherdream_enabled\":%s,"
                "\"ilw_enabled\":%s"
            "}"
        "}",
        cfg.idn_enabled ? "true" : "false",
        IDN_PROTOCOL_UDP_PORT,
        cfg.etherdream_enabled ? "true" : "false",
        ETHERDREAM_PROTOCOL_TCP_PORT,
        ETHERDREAM_PROTOCOL_UDP_PORT,
        cfg.ilw_enabled ? "true" : "false",
        ILW_PROTOCOL_UDP_PORT,
        protocol_settings_pending_reboot() ? "true" : "false",
        active.idn_enabled ? "true" : "false",
        active.etherdream_enabled ? "true" : "false",
        active.ilw_enabled ? "true" : "false"
    );

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

static esp_err_t protocol_config_post_handler(httpd_req_t *req)
{
    char body[384];
    esp_err_t ret = read_request_body(req, body, sizeof(body));

    if (ret != ESP_OK) {
        return send_bad_request(req, "Protocol settings form is too large");
    }

    protocol_settings_t cfg;
    protocol_settings_get(&cfg);

    char value[32];

    cfg.sd_ilda_enabled = true;

    if (form_get_value(body, "idn_enabled", value, sizeof(value))) {
        cfg.idn_enabled = form_value_enabled(value);
    } else {
        cfg.idn_enabled = false;
    }

    if (form_get_value(body, "etherdream_enabled", value, sizeof(value))) {
        cfg.etherdream_enabled = form_value_enabled(value);
    } else {
        cfg.etherdream_enabled = false;
    }

    if (form_get_value(body, "ilw_enabled", value, sizeof(value))) {
        cfg.ilw_enabled = form_value_enabled(value);
    } else {
        cfg.ilw_enabled = false;
    }

    ret = protocol_settings_save(&cfg);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    char json[64];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"reboot_required\":%s}",
             protocol_settings_pending_reboot() ? "true" : "false");

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

static esp_err_t system_reboot_post_handler(httpd_req_t *req)
{
    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t ret = httpd_resp_sendstr(req, "OK restarting");

    BaseType_t ok = xTaskCreate(
        reboot_task,
        "web_reboot",
        2048,
        NULL,
        3,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create reboot task");
    }

    return ret;
}

static esp_err_t system_factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t reset_ret = net_manager_restore_defaults();
    if (reset_ret != ESP_OK) {
        return send_plain_result(req, reset_ret);
    }

    reset_ret = protocol_settings_restore_defaults();
    if (reset_ret != ESP_OK) {
        return send_plain_result(req, reset_ret);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t ret = httpd_resp_sendstr(req, "OK factory defaults restored; restarting");

    BaseType_t ok = xTaskCreate(
        reboot_task,
        "web_factory",
        2048,
        NULL,
        3,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create factory reset reboot task");
    }

    return ret;
}

/* ===================== CONTROL HANDLERS ===================== */

static esp_err_t rp_refresh_post_handler(httpd_req_t *req)
{
    rp_manager_ping();
    return send_plain_result(req, rp_manager_get_remote_status());
}

static esp_err_t rp_ping_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, rp_manager_ping());
}

static esp_err_t rp_start_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, rp_manager_output_start());
}

static esp_err_t stop_everything_safe(void)
{
    esp_err_t ilda_ret = ilda_player_stop_and_wait(1500);
    rp_manager_stop_test_stream();
    ilw_protocol_stop();

    esp_err_t stop_ret = rp_manager_output_force_stop();
    esp_err_t clear_ret = rp_manager_clear_buffer();

    if (stop_ret != ESP_OK) {
        return stop_ret;
    }

    if (clear_ret != ESP_OK) {
        return clear_ret;
    }

    return ilda_ret;
}

static esp_err_t rp_stop_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, stop_everything_safe());
}

static esp_err_t rp_clear_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, stop_everything_safe());
}

static esp_err_t rp_start_stream_post_handler(httpd_req_t *req)
{
    char query[64];
    char shape[24];
    rp_test_shape_t test_shape = RP_TEST_SHAPE_SQUARE;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "shape", shape, sizeof(shape)) == ESP_OK) {
        if (strcasecmp(shape, "circle") == 0) {
            test_shape = RP_TEST_SHAPE_CIRCLE;
        } else if (strcasecmp(shape, "cross") == 0) {
            test_shape = RP_TEST_SHAPE_CROSS;
        } else if (strcasecmp(shape, "heart") == 0) {
            test_shape = RP_TEST_SHAPE_HEART;
        } else if (strcasecmp(shape, "colorshift") == 0) {
            test_shape = RP_TEST_SHAPE_COLOR_SHIFT;
        } else {
            test_shape = RP_TEST_SHAPE_SQUARE;
        }
    }

    ilda_player_stop();
    return send_plain_result(req, rp_manager_start_test_shape(test_shape));
}

static esp_err_t rp_test_color_post_handler(httpd_req_t *req)
{
    char query[64];
    char param[16];
    uint8_t mask = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "mask", param, sizeof(param)) == ESP_OK) {
        mask = (uint8_t)(strtoul(param, NULL, 10) & 0x07);
    }

    rp_manager_set_test_color_mask(mask);

    rp_status_t status;
    rp_manager_get_status(&status);

    if (mask != 0 && !status.test_stream_running) {
        return send_plain_result(req, rp_manager_start_test_shape(RP_TEST_SHAPE_BEAM));
    }

    return send_plain_result(req, ESP_OK);
}

static esp_err_t rp_stop_stream_post_handler(httpd_req_t *req)
{
    /* A Stop button leaves the scanners parked and the laser off, whatever started output. */
    return send_plain_result(req, stop_everything_safe());
}

static esp_err_t rp_set_rate_post_handler(httpd_req_t *req)
{
    char query[64];
    char param[32];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_sendstr(req, "ERR missing query");
    }

    if (httpd_query_key_value(query, "value", param, sizeof(param)) != ESP_OK) {
        return httpd_resp_sendstr(req, "ERR missing value");
    }

    uint32_t rate = (uint32_t)strtoul(param, NULL, 10);

    if (rate < 1) {
        rate = 1;
    }

    if (rate > 200000) {
        rate = 200000;
    }

    return send_plain_result(req, rp_manager_set_rate(rate));
}

static esp_err_t rp_rate_limit_post_handler(httpd_req_t *req)
{
    char query[64];
    char param[32];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "value", param, sizeof(param)) != ESP_OK) {
        return send_bad_request(req, "Scanner rate limit is required");
    }

    uint32_t limit = (uint32_t)strtoul(param, NULL, 10);

    if (limit < RP_MIN_RATE_LIMIT_HZ || limit > RP_MAX_RATE_LIMIT_HZ) {
        return send_bad_request(req, "Scanner rate limit must be between 1000 and 200000 pps");
    }

    return send_plain_result(req, rp_manager_set_rate_limit(limit));
}

static esp_err_t rp_color_shift_post_handler(httpd_req_t *req)
{
    char query[64];
    char param[32];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_bad_request(req, "Colour shift value is required");
    }

    /* ?save=1 writes the applied shift to RP flash; refused while running. */
    if (httpd_query_key_value(query, "save", param, sizeof(param)) == ESP_OK) {
        esp_err_t ret = rp_manager_save_color_shift();
        if (ret == ESP_ERR_INVALID_STATE) {
            return send_bad_request(req, "Stop output to save the colour shift");
        }
        return send_plain_result(req, ret);
    }

    if (httpd_query_key_value(query, "value", param, sizeof(param)) != ESP_OK) {
        return send_bad_request(req, "Colour shift value is required");
    }

    long shift_us = strtol(param, NULL, 10);

    if (shift_us < -2000 || shift_us > 2000) {
        return send_bad_request(req, "Colour shift must be between -2000 and 2000 microseconds");
    }

    return send_plain_result(req, rp_manager_set_color_shift((int32_t)shift_us));
}

static esp_err_t rp_buffer_config_post_handler(httpd_req_t *req)
{
    char body[160];
    char value[32];

    esp_err_t ret = read_request_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return send_bad_request(req, "DAC buffer settings form is too large");
    }

    if (!form_get_value(body, "size", value, sizeof(value))) {
        return send_bad_request(req, "Missing buffer size");
    }

    uint32_t size = (uint32_t)strtoul(value, NULL, 10);

    if (!form_get_value(body, "high_water", value, sizeof(value))) {
        return send_bad_request(req, "Missing high-water limit");
    }

    uint32_t high_water = (uint32_t)strtoul(value, NULL, 10);

    if (!form_get_value(body, "low_water", value, sizeof(value))) {
        return send_bad_request(req, "Missing low-water limit");
    }

    uint32_t low_water = (uint32_t)strtoul(value, NULL, 10);

    if (size < 512) {
        return send_bad_request(req, "Buffer size must be at least 512 samples");
    }

    if (high_water < 256) {
        return send_bad_request(req, "High-water limit must be at least 256 samples");
    }

    if (high_water > size - 256) {
        return send_bad_request(req, "High-water limit must leave at least 256 samples of headroom");
    }

    if (low_water > high_water - 256) {
        return send_bad_request(req, "Low-water limit must be at least 256 samples below high-water");
    }

    rp_status_t rp;
    rp_manager_get_status(&rp);

    if (rp.remote_buffer_max_samples > 0 && size > rp.remote_buffer_max_samples) {
        return send_bad_request(req, "Buffer size is above the RP maximum");
    }

    ilda_player_stop_and_wait(1500);

    ret = rp_manager_stop_test_stream();
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    ret = rp_manager_set_buffer_config(size, high_water, low_water);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t rp_mirror_config_post_handler(httpd_req_t *req)
{
    char body[96];
    char value[16];
    bool mirror_x = false;
    bool mirror_y = false;

    esp_err_t ret = read_request_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return send_bad_request(req, "DAC output mapping form is too large");
    }

    if (form_get_value(body, "mirror_x", value, sizeof(value))) {
        mirror_x = form_value_enabled(value);
    }

    if (form_get_value(body, "mirror_y", value, sizeof(value))) {
        mirror_y = form_value_enabled(value);
    }

    ret = rp_manager_set_mirror(mirror_x, mirror_y);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static bool parse_u32_in_range(const char *value, uint32_t min, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);

    if (value[0] == '\0' || !end || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *out = (uint32_t)parsed;
    return true;
}

static bool parse_scale_percent(const char *value, uint8_t *scale_percent)
{
    uint32_t parsed;

    if (!parse_u32_in_range(value, ILDA_SCALE_MIN_PERCENT, ILDA_SCALE_MAX_PERCENT, &parsed)) {
        return false;
    }

    *scale_percent = (uint8_t)parsed;
    return true;
}

static esp_err_t ilda_start_post_handler(httpd_req_t *req)
{
    char query[WEB_SD_PATH_MAX * 3 + 32];
    char path[WEB_SD_PATH_MAX * 3 + 1];
    char scale_value[8];
    uint8_t scale_percent = ILDA_SCALE_MAX_PERCENT;

    strlcpy(path, "/sdcard/test.ild", sizeof(path));

    /* Only a request with no path plays the default file; a cut-off path is an error. */
    esp_err_t ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (ret == ESP_OK) {
        ret = httpd_query_key_value(query, "path", path, sizeof(path));
        if (ret == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return send_bad_request(req, "SD file path is too long");
        }
        if (ret != ESP_OK) {
            strlcpy(path, "/sdcard/test.ild", sizeof(path));
        }

        if (httpd_query_key_value(query, "scale", scale_value, sizeof(scale_value)) == ESP_OK &&
            !parse_scale_percent(scale_value, &scale_percent)) {
            return send_bad_request(req, "XY scale must be between 10 and 100 percent");
        }
    } else if (ret == ESP_ERR_HTTPD_RESULT_TRUNC) {
        return send_bad_request(req, "SD file path is too long");
    }

    url_decode_inplace(path);

    size_t mount_len = strlen(SD_MOUNT_POINT);
    if (strncmp(path, SD_MOUNT_POINT "/", mount_len + 1) != 0 ||
        path[mount_len + 1] == '\0' ||
        !valid_sd_relative_path(path + mount_len + 1)) {
        return send_bad_request(req, "Invalid SD file path");
    }

    return send_plain_result(req, ilda_player_start(path, scale_percent));
}

static esp_err_t ilda_scale_post_handler(httpd_req_t *req)
{
    char query[32];
    char scale_value[8];
    uint8_t scale_percent;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "value", scale_value, sizeof(scale_value)) != ESP_OK) {
        return send_bad_request(req, "XY scale is required");
    }

    if (!parse_scale_percent(scale_value, &scale_percent)) {
        return send_bad_request(req, "XY scale must be between 10 and 100 percent");
    }

    return send_plain_result(req, ilda_player_set_scale(scale_percent));
}

static esp_err_t ilda_folder_post_handler(httpd_req_t *req)
{
    char query[WEB_SD_REL_PATH_MAX * 3 + 96];
    char encoded_path[WEB_SD_REL_PATH_MAX * 3 + 1];
    char value[16];
    char full_path[WEB_SD_PATH_MAX + 1];
    uint32_t hold_s = ILDA_FOLDER_HOLD_DEFAULT_S;
    uint32_t plays = ILDA_FOLDER_PLAYS_DEFAULT;
    uint8_t scale_percent = ILDA_SCALE_MAX_PERCENT;
    bool recursive = false;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", encoded_path, sizeof(encoded_path)) != ESP_OK) {
        return send_bad_request(req, "A folder path is required");
    }

    url_decode_inplace(encoded_path);
    if (!valid_sd_relative_path(encoded_path)) {
        return send_bad_request(req, "Invalid SD folder path");
    }

    if (httpd_query_key_value(query, "hold", value, sizeof(value)) == ESP_OK &&
        !parse_u32_in_range(value, 1, ILDA_FOLDER_HOLD_MAX_S, &hold_s)) {
        return send_bad_request(req, "Hold time must be between 1 and 3600 seconds");
    }

    if (httpd_query_key_value(query, "plays", value, sizeof(value)) == ESP_OK &&
        !parse_u32_in_range(value, 1, ILDA_FOLDER_PLAYS_MAX, &plays)) {
        return send_bad_request(req, "Animation plays must be between 1 and 1000");
    }

    if (httpd_query_key_value(query, "scale", value, sizeof(value)) == ESP_OK &&
        !parse_scale_percent(value, &scale_percent)) {
        return send_bad_request(req, "XY scale must be between 10 and 100 percent");
    }

    if (httpd_query_key_value(query, "recursive", value, sizeof(value)) == ESP_OK) {
        recursive = form_value_enabled(value);
    }

    int path_len = encoded_path[0]
        ? snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, encoded_path)
        : snprintf(full_path, sizeof(full_path), "%s", SD_MOUNT_POINT);
    if (path_len < 0 || (size_t)path_len >= sizeof(full_path)) {
        return send_bad_request(req, "SD folder path is too long");
    }

    return send_plain_result(req, ilda_player_start_folder(full_path, recursive,
                                                           hold_s * 1000U,
                                                           (uint16_t)plays,
                                                           scale_percent));
}

static esp_err_t ilda_stop_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, stop_everything_safe());
}

/* ===================== UI ===================== */

static const char *html_page =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ILDAWaveX16</title>"
"<style>"
":root{--bg:#1f2429;--side:#191e23;--top:#20262b;--panel:#20262b;--panel2:#262c32;--line:#3a424a;--text:#eef3f7;--muted:#9aa5ad;--green:#15985c;--green2:#1dbf73;--red:#d94b4b;--amber:#d99b28;--blue:#4aa3ff}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,-apple-system,'Helvetica Neue',Arial,sans-serif;font-size:14px}"
".app{display:flex;min-height:100vh}.side{width:292px;background:var(--side);position:fixed;inset:0 auto 0 0;padding:18px 10px;display:flex;flex-direction:column;border-right:1px solid #15191d}"
".brand{padding:0 10px 20px}.brand h1{margin:0;font-size:24px;line-height:1.08;font-weight:900;letter-spacing:0;background:linear-gradient(90deg,#e35d5b,#f0b44c,#47c97a,#4aa3ff,#a77bff,#e35d5b);background-size:500% 500%;-webkit-background-clip:text;background-clip:text;color:transparent;animation:rainbowShift 9s ease-in-out infinite;white-space:nowrap}.brand p{margin:5px 0 0;color:#dbe3ea;font-size:12px}"
"@keyframes rainbowShift{0%{background-position:0% 50%}50%{background-position:100% 50%}100%{background-position:0% 50%}}"
".nav button{width:100%;height:39px;border:0;border-radius:6px;background:transparent;color:#d8e0e6;text-align:left;padding:0 13px;margin:2px 0;cursor:pointer;font-weight:600;font-family:inherit}.nav button:hover{background:#242b31}.nav button.active{background:var(--green);color:white}"
".main{margin-left:292px;width:calc(100% - 292px);min-height:100vh;display:flex;flex-direction:column}.top{min-height:62px;background:var(--top);display:flex;align-items:center;justify-content:space-between;padding:9px 18px;border-bottom:1px solid #171b20;gap:14px;font-size:12px}"
".topStatus{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.topItem{display:flex;align-items:center;gap:6px;background:#1a2025;border:1px solid var(--line);border-radius:5px;padding:7px 9px;white-space:nowrap}.topItem.ok{border-color:var(--green);color:var(--green2);font-weight:700}.topItem.bad{border-color:var(--red);color:var(--red);font-weight:700}.topActions{display:flex;align-items:center;gap:7px;flex-wrap:wrap;justify-content:flex-end}.top b{color:#fff}.tag{border:1px solid var(--green);color:var(--green2);border-radius:4px;padding:2px 7px;font-weight:700}.tag.warn{border-color:var(--amber);color:var(--amber)}.tag.bad{border-color:var(--red);color:var(--red)}"
".bufferTop{padding:5px 7px}.bufferMeter{position:relative;width:180px;height:26px;border:1px solid var(--line);border-radius:4px;overflow:hidden;background:#0e1317}.bufferFill{position:absolute;left:0;top:0;bottom:0;background:rgba(74,163,255,.45);transition:width .25s,background .3s}.bufferMark{position:absolute;top:0;bottom:0;width:2px}.bufferMark.low{background:#22e882}.bufferMark.high{background:#f0b44c}.bufferText{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-weight:700;font-size:11px;color:#fff;text-shadow:0 1px 3px rgba(0,0,0,.95);font-family:'Consolas','SF Mono','Roboto Mono',monospace;letter-spacing:.02em}"
".startBtn{background:var(--green)!important;color:#fff!important;font-weight:900;border:0!important;border-radius:6px;padding:9px 12px!important;font-family:inherit}.stopBtn{background:var(--red)!important;color:#fff!important;font-weight:900;border:0!important;border-radius:6px;padding:9px 12px!important;font-family:inherit}.refreshBtn{background:#2d3740;color:#cdd8e0;border:1px solid var(--line);border-radius:6px;padding:9px 12px;cursor:pointer;font-family:inherit;font-size:12px;font-weight:700}.refreshBtn:hover{filter:brightness(1.18)}.content{padding:26px 34px;flex:1}.crumb{font-size:12px;color:#b8c2ca;margin-bottom:13px}.crumb span{color:var(--green2)}"
".section{display:none}.section.active{display:block}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:18px}.card{background:var(--panel);border:1px solid var(--line);border-radius:5px;margin-bottom:18px}"
".card h3{font-size:14px;margin:0;padding:13px 15px;border-bottom:1px solid var(--line);font-weight:700}.cardHead{display:flex;align-items:center;justify-content:space-between;gap:12px;border-bottom:1px solid var(--line);padding:8px 15px}.cardHead h3{padding:0;border-bottom:0}.rows{padding:8px 15px 15px}.rowkv{display:grid;grid-template-columns:1.2fr 1fr;gap:10px;border-bottom:1px solid var(--line);padding:9px 0}.rowkv:last-child{border-bottom:0}.k{font-weight:700;color:#fff}.v{color:#fff;font-variant-numeric:tabular-nums}.muted{color:var(--muted)}"
".status-ok{color:var(--green2)}.status-warn{color:var(--amber)}.status-bad{color:var(--red)}button{background:#2d3740;color:#fff;border:1px solid var(--line);border-radius:5px;padding:9px 12px;margin:3px;cursor:pointer;font-family:inherit}button:hover{filter:brightness(1.12)}button.green{background:var(--green);border-color:var(--green)}button.red{background:var(--red);border-color:var(--red)}button.blue{background:#246aa5;border-color:#246aa5}button.toggle{min-width:68px;font-weight:600;transition:background .15s,border-color .15s,color .15s,opacity .15s}button.toggle.red{background:#2d1a1a;border-color:#6a2a2a;color:#f07070}button.toggle.green{background:#1a2d1a;border-color:#2a6a2a;color:#52e899}button.toggle.blue{background:#1a1a2d;border-color:#2a2a6a;color:#6ab0ff}button.toggle.black{background:#101418;border-color:#3a424a;color:#8a9499}button.toggle.active.red{background:#b03030;border-color:#d04040;color:#fff}button.toggle.active.green{background:var(--green);border-color:#1dbf73;color:#fff}button.toggle.active.blue{background:#1e5a95;border-color:#4aa3ff;color:#fff}button.toggle.active.black{background:#252c33;border-color:#5a6570;color:#dce4ea}.rateGrid{display:grid;grid-template-columns:repeat(5,minmax(70px,1fr));gap:8px;margin-bottom:9px}.rateGrid button{margin:0}"
"input,select{background:#171c21;color:#fff;border:1px solid var(--line);border-radius:5px;padding:8px;width:100%;font-family:inherit}.actions{padding:12px 15px}.pill{display:inline-block;border:1px solid var(--line);border-radius:4px;padding:3px 7px;margin:2px;color:#dce4ea}.inlineForm{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center;margin-top:10px}"
".checkrow{display:flex;align-items:center;gap:9px;padding:4px 0}.checkrow input{width:auto}.fieldrow{display:grid;grid-template-columns:1.2fr 1fr;gap:10px;align-items:center;border-bottom:1px solid var(--line);padding:9px 0}.fieldrow:last-child{border-bottom:0}.ipFields{display:none}.saveMsg{min-height:20px;color:var(--muted);padding:0 15px 12px}.saveMsg.ok{color:var(--green2)}.saveMsg.err{color:var(--red)}.note{color:var(--muted);font-size:12px;line-height:1.4;padding:0 15px 12px}"
".fileTools{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.fileTools button{margin:0}.tableWrap{overflow:auto;padding:0 15px 15px}.tableWrap table{min-width:100%}"
".sdTable .previewBtn{margin:0 5px 0 0;padding:6px 9px}.sdTable tr.previewSelected td{background:#28332f}"
".previewGrid{display:grid;grid-template-columns:minmax(0,1.5fr) minmax(240px,1fr);gap:16px;padding:15px}.previewCanvasWrap{background:#080b0d;border:1px solid var(--line);border-radius:4px;overflow:hidden}.previewCanvasWrap canvas{display:block;width:100%;height:auto;aspect-ratio:8/5}.previewSummary{min-width:0}.previewSummary .rowkv{grid-template-columns:1fr auto;gap:8px}.previewVerdict{padding:12px 0 0;font-weight:700}.previewVerdict.ok{color:var(--green2)}.previewVerdict.warn{color:var(--amber)}.previewVerdict.err{color:var(--red)}.previewFrameLabel{min-width:90px;text-align:center;font-variant-numeric:tabular-nums}"
".folderPlay{display:flex;flex-wrap:wrap;align-items:center;gap:8px 14px;padding-top:0}.folderPlay button{margin:0}.folderPlay label{display:flex;align-items:center;gap:6px;color:var(--muted)}.folderPlay input[type=number]{width:72px;padding:6px}.folderPlay input[type=checkbox]{width:auto}"
".scaleControl{display:flex;align-items:center;gap:10px}.scaleControl input{width:100%;padding:0}.scaleControl output{min-width:44px;text-align:right;font-variant-numeric:tabular-nums}.previewSummary .scaleRow{grid-template-columns:1fr;gap:7px}"
"table{width:100%;border-collapse:collapse}.card>table{width:calc(100% - 30px);margin:0 15px 15px}.sdTable{min-width:420px}.sdTable .sizeCol{width:92px}.sdTable .actionCol{width:150px;text-align:right}.sdTable .sizeCell{white-space:nowrap;color:#dce4ea}.sdTable .actionCell{text-align:right}.sdTable .playBtn{margin:0;padding:6px 10px}th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line)}th{font-size:12px;color:#dce4ea}.bufInputCol{width:140px}.bufInfoCol{width:120px;white-space:nowrap}.bufInfo{color:var(--muted);font-variant-numeric:tabular-nums}.bufDesc{font-size:12px}.bufActions{display:flex;align-items:center;gap:4px}.bufActions .saveMsg{padding:0 8px;margin:0}"
"pre{margin:0;background:#14191e;color:#9dffbc;padding:13px;max-height:420px;overflow:auto;font-size:12px}.footer{margin-top:auto;border-top:1px solid #171b20;background:#1a2025;text-align:center;padding:14px 16px;font-size:12px}.footerLink{color:#fff;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}.footerLink:hover{color:#fff}.footerLogo{width:112px;height:auto;display:block}"
"@media(max-width:900px){.app{display:block}.side{position:sticky;top:0;z-index:5;width:100%;padding:10px 12px;display:block;border-right:0;border-bottom:1px solid #15191d}.brand{padding:0 4px 10px}.brand h1{font-size:clamp(20px,7vw,25px)}.brand p{display:none}.nav{display:flex;gap:6px;overflow-x:auto;padding-bottom:2px}.nav button{flex:0 0 auto;width:auto;height:36px;padding:0 11px;margin:0;white-space:nowrap}.main{margin-left:0;width:100%}.top{height:auto;align-items:flex-start;flex-direction:column;padding:10px}.topActions{justify-content:flex-start}.grid2{grid-template-columns:1fr}.rateGrid{grid-template-columns:repeat(2,minmax(0,1fr))}.content{padding:16px}.rowkv,.fieldrow{grid-template-columns:1fr;gap:5px}}"
"</style>"
"</head>"
"<body>"
"<div class='app'>"

"<aside class='side'>"
"<div class='brand'><h1 id='brandName'>ILDAWaveX16</h1><p>Open-source all-in-one laser DAC</p></div>"
"<div class='nav'>"
"<button class='active' data-tab='dashboard' onclick='showTab(\"dashboard\",this)'>Dashboard</button>"
"<button data-tab='playback' onclick='showTab(\"playback\",this)'>SD Playback</button>"
"<button data-tab='protocols' onclick='showTab(\"protocols\",this)'>Protocols</button>"
"<button data-tab='network' onclick='showTab(\"network\",this)'>Network</button>"
"<button data-tab='diagnostics' onclick='showTab(\"diagnostics\",this)'>Diagnostics</button>"
"<button data-tab='system' onclick='showTab(\"system\",this)'>System</button>"
"</div>"
"</aside>"

"<main class='main'>"
"<div class='top'>"
"<div class='topStatus'>"
"<span class='topItem' id='topConn'>DAC Engine</span>"
"<span class='topItem' id='topState'>DAC Output</span>"
"<span class='topItem'><b id='topRate'>-- kpps</b></span>"
"<span class='topItem bufferTop' title='Ring buffer fill'><span class='bufferMeter'><span class='bufferFill' id='topBufferFill'></span><span class='bufferMark low' id='topBufLowMark' title='Low-water'></span><span class='bufferMark high' id='topBufHighMark' title='High-water'></span><span class='bufferText' id='topFill'>--</span></span></span>"
"</div>"
"<div class='topActions'>"
"<button class='refreshBtn' onclick='refreshEngine()' title='Refresh DAC Engine connection'>Refresh</button>"
"<button id='topStartBtn' class='startBtn' onclick='post(\"/api/rp/start\")'>START</button>"
"<button id='topStopBtn' class='stopBtn' onclick='stopAll()'>STOP</button>"
"</div>"
"</div>"

"<div class='content'>"
"<div class='crumb'>ILDAWaveX16 / <span id='crumb'>Dashboard</span></div>"

"<section id='dashboard' class='section active'>"
"<div class='grid2'>"
"<div class='card'><h3>Output health</h3><div class='rows'>"
"<div class='rowkv'><div class='k'>Underruns / second:</div><div class='v' id='dashUnderrunRate'>--</div></div>"
"<div class='rowkv'><div class='k'>Payload errors / second:</div><div class='v' id='dashPayloadRate'>--</div></div>"
"</div></div>"
"<div class='card'><h3>Protocol input</h3>"
"<div class='tableWrap'><table><thead><tr><th>Protocol</th><th>Samples / second</th><th>Status</th></tr></thead><tbody>"
"<tr><td>SD ILDA playback</td><td class='v' id='protoRateSd'>0</td><td id='protoStateSd' class='muted'>Idle</td></tr>"
"<tr><td>ILDA IDN</td><td class='v' id='protoRateIdn'>0</td><td id='protoStateIdn' class='muted'>Idle</td></tr>"
"<tr><td>Ether Dream</td><td class='v' id='protoRateEther'>0</td><td id='protoStateEther' class='muted'>Idle</td></tr>"
"<tr><td>ILW-UDP</td><td class='v' id='protoRateIlw'>0</td><td id='protoStateIlw' class='muted'>Idle</td></tr>"
"</tbody></table></div></div>"
"</div>"

"<div class='card'><h3>DAC Sample Rate</h3><div class='actions'>"
"<div class='rateGrid'>"
"<button class='blue rateBtn' data-rate='10000' onclick='setRate(10000)'>10 kpps</button>"
"<button class='blue rateBtn' data-rate='20000' onclick='setRate(20000)'>20 kpps</button>"
"<button class='blue rateBtn' data-rate='30000' onclick='setRate(30000)'>30 kpps</button>"
"<button class='blue rateBtn' data-rate='40000' onclick='setRate(40000)'>40 kpps</button>"
"<button class='blue rateBtn' data-rate='50000' onclick='setRate(50000)'>50 kpps</button>"
"</div>"
"<div class='inlineForm'><input id='rateInput' type='number' value='30000' min='1' max='200000'><button onclick='setCustomRate()'>Set custom</button></div>"
"<div class='inlineForm'><label for='rateLimitPreset'>Scanner rating</label><select id='rateLimitPreset' onchange='onRateLimitPreset()'><option value=''>Custom</option><option value='12000'>12K scanners</option><option value='20000'>20K scanners</option><option value='30000'>30K scanners</option><option value='40000'>40K scanners</option><option value='50000'>50K scanners</option></select></div>"
"<div class='inlineForm'><label for='rateLimitInput'>Scanner limit (pps)</label><input id='rateLimitInput' type='number' min='1000' max='200000' oninput='rateLimitDirty=true;matchRateLimitPreset(this.value)'><button class='green' onclick='saveRateLimit()'>Save limit</button><span id='rateLimitMsg' class='saveMsg'></span></div>"
"<div class='note' style='padding:8px 0 0'>Every requested rate, from any protocol or this page, is clamped to the scanner limit. Pick your galvos' rating, or enter a custom limit, then Save limit. Ratings are at 8&deg; optical on the ILDA test pattern; at wider angles use a lower limit.</div>"
"</div></div>"

"<div class='card'><h3>DAC Output Mapping</h3>"
"<div class='rows'>"
"<div class='fieldrow'><div class='k'>Mirror X</div><div class='v'><label class='checkrow'><input id='mirrorX' type='checkbox' onchange='onMirrorInput()'>Enabled</label></div></div>"
"<div class='fieldrow'><div class='k'>Mirror Y</div><div class='v'><label class='checkrow'><input id='mirrorY' type='checkbox' onchange='onMirrorInput()'>Enabled</label></div></div>"
"</div>"
"<div class='actions bufActions'><button class='green' onclick='saveMirrorSettings()'>Save</button><span id='mirrorSaveMsg' class='saveMsg'>Applies immediately and saves to DAC Engine flash.</span></div>"
"<div class='rows' style='margin-top:10px'><div class='fieldrow'><div class='k'>Colour shift</div><div class='v'><div class='inlineForm'><input id='colorShiftInput' type='number' min='-2000' max='2000' step='5' oninput='colorShiftDirty=true'><span class='muted'>&micro;s</span><button onclick='applyColorShift()'>Apply</button><button class='green' onclick='saveColorShift()'>Save</button></div>"
"<div class='actions' style='padding:8px 0 0'><button onclick='post(\"/api/rp/start_test_stream?shape=colorshift\")'>Show pattern</button><button class='red' onclick='post(\"/api/rp/stop_test_stream\")'>Stop pattern</button></div>"
"<div class='actions' style='padding:8px 0 0'><button onclick='stepColorShift(-10)'>&minus;10</button><button onclick='stepColorShift(-1)'>&minus;1</button><button onclick='stepColorShift(1)'>+1</button><button onclick='stepColorShift(10)'>+10</button></div>"
"<div><span id='colorShiftMsg' class='saveMsg'></span></div>"
"<div class='note' style='padding:8px 0 0'>Press Show pattern. Each red, green and blue column has a white line ending at it above and one starting at it below. If the upper line stops short and the lower one crosses, raise the shift; if the upper one crosses and the lower one leaves a gap, lower it. When both just touch, press Save: it stops the pattern, then saves. While output runs, changes apply at once but are not kept after a restart until saved.</div></div></div></div>"
"<div class='note' style='padding:8px 0 0'>Delays the colour channels behind the scanner position (positive) so lit segments start and end where the beam really is. Applies to every source, including SD playback.</div></div>"

"<div class='card'><h3>DAC Buffer</h3>"
"<table><thead><tr><th>Setting</th><th class='bufInputCol'>Value</th><th class='bufInfoCol'>Active / Limit</th><th>Description</th></tr></thead>"
"<tbody>"
"<tr><td class='k'>Buffer size</td><td><input id='bufSizeInput' inputmode='numeric' pattern='[0-9]*' oninput='onBufInput()'></td><td id='bufSizeInfo' class='bufInfo'>--</td><td class='bufDesc muted'>Total ring buffer capacity on the DAC Engine.</td></tr>"
"<tr><td class='k'>High-water</td><td><input id='bufHighInput' inputmode='numeric' pattern='[0-9]*' oninput='onBufInput()'></td><td id='bufHighInfo' class='bufInfo'>--</td><td class='bufDesc muted'>Stream pauses when fill reaches this level.</td></tr>"
"<tr><td class='k'>Low-water</td><td><input id='bufLowInput' inputmode='numeric' pattern='[0-9]*' oninput='onBufInput()'></td><td id='bufLowInfo' class='bufInfo'>--</td><td class='bufDesc muted'>Stream resumes when fill drains back to this level.</td></tr>"
"</tbody></table>"
"<div class='actions bufActions'><button class='green' onclick='saveBufferSettings()'>Save</button><button id='bufCancelBtn' onclick='cancelBufEdit()' style='display:none'>Revert</button><span id='bufSaveMsg' class='saveMsg'>Applies immediately and saves to RP flash.</span></div></div>"
"</section>"

"<section id='playback' class='section'>"
"<div class='card filesCard'><div class='cardHead'><h3>SD playback</h3><button class='refreshBtn' onclick='loadSdFiles()'>Rescan SD Card</button></div>"
"<div class='actions' style='padding-top:0'><button id='sdUpBtn' onclick='goSdParent()'>Up</button><span id='sdCurrentPath' class='muted'>/</span></div>"
"<div class='actions folderPlay'><button id='sdPlayFolderBtn' class='green' onclick='playSdFolder()'>Play folder</button><label>Hold single frames <input id='folderHold' type='number' min='1' max='3600' step='1' value='5'> s</label><label>Play animations <input id='folderPlays' type='number' min='1' max='1000' step='1' value='1'> times</label><label><input id='folderRecursive' type='checkbox'> Include subfolders</label><span id='folderPlayMsg' class='muted'></span></div>"
"<div class='tableWrap'><table class='sdTable'><thead><tr><th>Name</th><th class='sizeCol'>Size</th><th class='actionCol'>Action</th></tr></thead><tbody id='fileRows'></tbody></table></div>"
"</div>"
"<div class='card' id='ildaPreviewCard' style='display:none'><div class='cardHead'><h3 id='ildaPreviewTitle'>ILDA preview</h3><div class='fileTools'><button id='previewProjectBtn' class='green' onclick='startPreviewProject()' disabled>Project</button><button id='previewStopBtn' class='red' onclick='stopPreviewProject()' disabled>Stop</button><button id='previewPrevBtn' onclick='stepPreviewFrame(-1)' aria-label='Previous frame'>Previous</button><button id='previewPlayBtn' onclick='togglePreviewPlayback()' aria-label='Pause preview' aria-pressed='true'>Pause</button><span id='previewFrameLabel' class='previewFrameLabel muted'>Frame --</span><button id='previewNextBtn' onclick='stepPreviewFrame(1)' aria-label='Next frame'>Next</button></div></div>"
"<div class='previewGrid'><div class='previewCanvasWrap'><canvas id='ildaPreviewCanvas' width='640' height='400' aria-label='Selected ILDA frame preview'></canvas></div><div class='previewSummary'>"
"<div class='rowkv'><span class='k'>File size</span><span id='previewFileSize' class='v'>--</span></div>"
"<div class='rowkv'><span class='k'>Frames</span><span id='previewFrameCount' class='v'>--</span></div>"
"<div class='rowkv'><span class='k'>Total points</span><span id='previewTotalPoints' class='v'>--</span></div>"
"<div class='rowkv'><span class='k'>Largest frame</span><span id='previewMaxFramePoints' class='v'>--</span></div>"
"<div class='rowkv'><span class='k'>Frame points</span><span id='previewSelectedPoints' class='v'>--</span></div>"
"<div class='rowkv'><span class='k'>Formats</span><span id='previewFormats' class='v'>--</span></div>"
"<div class='fieldrow scaleRow'><label class='k' for='ildaScale'>Output XY scale</label><div class='scaleControl'><input id='ildaScale' type='range' min='10' max='100' step='1' value='100' oninput='onIldaScaleChange()'><output id='ildaScaleValue' for='ildaScale'>100%</output></div></div>"
"<div class='note' style='padding:8px 0 0'>Reduced scale keeps the pattern centered and narrows its scan angle.</div>"
"<div id='previewVerdict' class='previewVerdict muted'>Select Preview on an ILDA file.</div></div></div></div>"
"</section>"

"<section id='protocols' class='section'>"
"<div class='card'><h3>Input protocols</h3>"
"<div class='tableWrap'><table><thead><tr><th>Protocol</th><th>Enable</th><th>Status</th><th>Information</th></tr></thead><tbody>"
"<tr><td>SD ILDA playback</td><td><label class='checkrow'><input id='protoSdEnabled' type='checkbox' checked disabled>On</label></td><td class='v status-ok'>Enabled</td><td>Local microSD slot</td></tr>"
"<tr><td>ILDA IDN</td><td><label class='checkrow'><input id='protoIdnEnabled' type='checkbox'>On</label></td><td class='muted' id='protoIdnActive'>--</td><td><span id='protoIdnTransport'>UDP</span> port <span id='protoIdnPort'>--</span></td></tr>"
"<tr><td>Ether Dream</td><td><label class='checkrow'><input id='protoEtherEnabled' type='checkbox'>On</label></td><td class='muted' id='protoEtherActive'>--</td><td>TCP port <span id='protoEtherTcp'>--</span>, UDP discovery port <span id='protoEtherUdp'>--</span></td></tr>"
"<tr><td>ILW-UDP</td><td><label class='checkrow'><input id='protoIlwEnabled' type='checkbox'>On</label></td><td class='muted' id='protoIlwActive'>--</td><td><span id='protoIlwTransport'>UDP</span> port <span id='protoIlwPort'>--</span></td></tr>"
"</tbody></table></div>"
"<div class='actions'><button class='green' onclick='saveProtocolSettings()'>Save protocol settings</button><button onclick='restartDevice()'>Reboot</button></div><div id='protoSaveMsg' class='saveMsg'>Settings are saved to flash and applied on boot.</div></div>"
"</section>"

"<section id='network' class='section'>"
"<div class='grid2'>"
"<div class='card'><h3>Ethernet / LAN</h3><div class='rows'>"
"<div class='rowkv'><div class='k'>Ethernet link:</div><div class='v' id='netEthState'>--</div></div>"
"<div class='rowkv'><div class='k'>Ethernet IP:</div><div class='v' id='netEthIp'>--</div></div>"
"<div class='fieldrow'><label class='k' for='netHostname'>Hostname:</label><div class='v'><input id='netHostname' maxlength='31'></div></div>"
"<div class='fieldrow'><label class='k' for='netIpMode'>IP address:</label><div class='v'><select id='netIpMode' onchange='updateIpFields()'><option value='dhcp'>Automatic (DHCP)</option><option value='static'>Manual (static)</option></select></div></div>"
"<div id='netIpFields' class='ipFields'>"
"<div class='fieldrow'><label class='k' for='netStaticIp'>Static IP:</label><div class='v'><input id='netStaticIp' inputmode='numeric'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaticMask'>Netmask:</label><div class='v'><input id='netStaticMask' inputmode='numeric'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaticGateway'>Gateway:</label><div class='v'><input id='netStaticGateway' inputmode='numeric'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaticDns'>DNS server:</label><div class='v'><input id='netStaticDns' inputmode='numeric'></div></div>"
"</div>"
"</div></div>"
"<div class='card'><h3>Wi-Fi access point</h3><div class='rows'>"
"<div class='fieldrow'><div class='k'>Access point:</div><div class='v'><label class='checkrow'><input id='netApEnabled' type='checkbox'>Enabled</label></div></div>"
"<div class='rowkv'><div class='k'>AP IP:</div><div class='v' id='netApIp'>--</div></div>"
"<div class='fieldrow'><label class='k' for='netApSsid'>AP name:</label><div class='v'><input id='netApSsid' maxlength='32'></div></div>"
"<div class='fieldrow'><label class='k' for='netApPass'>AP password:</label><div class='v'><input id='netApPass' type='password' maxlength='63' placeholder='Leave blank to keep current'></div></div>"
"</div></div>"
"</div>"
"<div class='grid2'>"
"<div class='card'><h3>Wi-Fi station</h3><div class='rows'>"
"<div class='fieldrow'><div class='k'>Station mode:</div><div class='v'><label class='checkrow'><input id='netStaEnabled' type='checkbox'>Enabled</label></div></div>"
"<div class='rowkv'><div class='k'>Station status:</div><div class='v' id='netStaState'>--</div></div>"
"<div class='rowkv'><div class='k'>Station IP:</div><div class='v' id='netStaIp'>--</div></div>"
"<div class='fieldrow'><label class='k' for='netStaSsid'>Network name:</label><div class='v'><input id='netStaSsid' maxlength='32'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaPass'>Network password:</label><div class='v'><input id='netStaPass' type='password' maxlength='63' placeholder='Leave blank to keep current'></div></div>"
"</div></div>"
"<div class='card'><h3>Apply settings</h3><div class='actions'>"
"<button class='green' onclick='saveNetworkSettings()'>Save network settings</button>"
"<button onclick='restartDevice()'>Reboot</button>"
"</div><div id='netSaveMsg' class='saveMsg'>Settings are saved to flash and applied on boot.</div>"
"<div class='note'>Changing Wi-Fi or IP settings can move this page to a new address after reboot.</div></div>"
"</div>"
"</section>"

"<section id='diagnostics' class='section'>"
"<div class='card'><h3>Diagnostic counters</h3>"
"<div class='tableWrap'><table><thead><tr><th>Name</th><th>Number</th><th>Information</th></tr></thead><tbody>"
"<tr><td>Stream packets</td><td class='v' id='diagPackets'>--</td><td class='muted'>Valid SPI sample packets sent from the ESP32 to the DAC Engine.</td></tr>"
"<tr><td>Payload errors</td><td class='v' id='diagPayloadErrors'>--</td><td class='muted'>Payload checksum or framing errors reported by the DAC Engine.</td></tr>"
"<tr><td>Underruns</td><td class='v' id='diagUnderruns'>--</td><td class='muted'>Times the DAC Engine needed a sample but its buffer was empty.</td></tr>"
"<tr><td>Bad packets</td><td class='v' id='diagBadPackets'>--</td><td class='muted'>Malformed stream packets rejected before use.</td></tr>"
"<tr><td>Dropped packets</td><td class='v' id='diagDroppedPackets'>--</td><td class='muted'>Packets dropped because the DAC Engine could not accept them.</td></tr>"
"<tr><td>Idle RX wait timeouts</td><td class='v' id='diagRxTimeouts'>--</td><td class='muted'>Times the DAC Engine receiver armed itself for SPI input while no ESP32 stream packet arrived. This can increase while DAC output is stopped.</td></tr>"
"</tbody></table></div><div class='actions'><button onclick='resetDiagnosticCounters()'>Reset counters</button></div></div>"
"<div class='grid2'>"
"<div class='card'><h3>Status JSON</h3><pre id='status'>Loading...</pre></div>"
"<div class='card'><h3>Test Stream</h3>"
"<div class='actions'>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=square\")'>Square</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=circle\")'>Circle</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=cross\")'>Cross</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=heart\")'>Heart</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=colorshift\")'>Colour Shift</button>"
"</div>"
"<div class='actions' style='padding-top:0'>"
"<button id='testRedBtn' class='toggle red' onclick='toggleTestColor(1)'>Red</button>"
"<button id='testGreenBtn' class='toggle green' onclick='toggleTestColor(2)'>Green</button>"
"<button id='testBlueBtn' class='toggle blue' onclick='toggleTestColor(4)'>Blue</button>"
"<button id='testBlackBtn' class='toggle black' onclick='setTestBlack()'>Black</button>"
"</div>"
"<div class='actions' style='padding-top:0'>"
"<button class='red' onclick='post(\"/api/rp/stop_test_stream\")'>Stop Stream</button>"
"</div></div>"
"</div>"
"</section>"

"<section id='system' class='section'>"
"<div class='grid2'>"
"<div class='card'><h3>Firmware</h3><div class='rows'>"
"<div class='rowkv'><div class='k'>Main (ESP32-S3):</div><div class='v' id='aboutEspFw'>--</div></div>"
"<div class='rowkv'><div class='k'>DAC Engine (RP2350):</div><div class='v' id='aboutRpFw'>--</div></div>"
"</div></div>"
"<div class='card'><h3>Maintenance</h3><div class='rows'>"
"</div><div class='actions'><button class='red' onclick='factoryReset()'>Factory Reset</button><button onclick='restartDevice()'>Reboot</button></div><div id='systemMsg' class='saveMsg'></div></div>"
"</div>"
"</section>"
"</div>"
"<div class='footer'><a class='footerLink' href='https://stanleyprojects.com' target='_blank' rel='noopener' aria-label='StanleyProjects'><svg class='footerLogo' xmlns='http://www.w3.org/2000/svg' width='400' height='102.7' fill='currentColor' viewBox='0 0 300 77' aria-hidden='true'><path d='M.7.7H300V77H.8zm6.8 6.8V70h285.3V7.5zM23 22c2.2-4.8 9.7-6.3 13.3-2.3 2.4 4 .5 8.8.2 13.1h-6.6c.5-3.5 1.4-7 .3-10.5q-3 5-2.4 10.8c1.8 3.8 7.6 5 7.4 9.8-.2 4.5-.6 9-2 13.3-2.3 5.5-11.8 7.2-14.6 1.5-1.3-4.6.3-9.3.8-13.8H26c-.5 3.7-1.4 7.5-1.2 11.3 1.7.9 2.4-1.1 2.8-2.3.6-3 1.2-6 .8-9-1.8-3.7-7.1-5.2-7.2-9.9 0-4 .2-8.2 1.9-12m18-3.9h15.7l-.7 5.7-4.6.1-5.2 36.6h-6.6L44.8 24h-4.6zm19.2 0h13.5l-3.5 42.4h-6.5q.7-6.4 1.1-13l-4 .1-2.4 13h-6.7zM62 41.7h3.4c.2-6.3 2.4-12.8.7-19.1-2.3 6.1-2.8 12.7-4.1 19m16-23.5 6.5-.1L86 36q1.5-9 2.7-18H95l-5.8 42.4h-6.5L81 41.6q-1.4 9.4-2.7 19l-6.3-.1zm13.9 42.3 5.9-42.4h6.7l-5 36.5h4.8l-.8 6zm19-42.4h13l-.7 5.8h-6.5l-1.4 10.7h4.9l-.8 6h-5l-2 14h6.6c-.5 1.9-.4 4-1.3 5.8-4.2.3-8.5 0-12.7 0zm14.3 0h6.5q.2 6.1.8 12.2l3.7-12.2h6.2l-7 19.3c-3 7.3-3 15.4-4.4 23l-6.7.1c.9-7.8 3-15.6 2.2-23.5zm19 .2c4.2-.3 8.7-1.3 12.7.6 2.6 1.8 2.3 5.4 2.1 8.2-.5 4.5-1 9.4-3.7 13.2-2 2.4-5.2 2.5-8 3L145 60.5h-6.7zm4.1 19.6c4-3.8 5-10.4 2.5-15.2-1.7 4.9-2 10-2.5 15.2m14.3-19.6c4-.3 8.4-1.3 12.2.7 3.5 2.2 2.9 6.8 2.4 10.3-.2 3.1-1.4 6.2-3.7 8.4.9 2 2 4 1.7 6.2-.5 5.6-1.6 11-2.3 16.6h-6.6c.7-5.5 1.8-11 2.1-16.5.4-1.7-1-2.8-2.3-3.4l-2.8 20-6.7-.1zM167 36c3.8-3 5-9.5 1.9-13.2q-1.5 6.5-1.9 13.2m18.2-17.5c4-2.3 11-1 11.2 4.5-.4 9-2.4 18-3.4 27-.3 4.2-1.7 9.1-6.2 10.6-4.1 1.9-10.5-.3-10.2-5.5.5-8.4 2.3-16.7 3.2-25.1.5-4.2 1-9.5 5.4-11.5m2.4 5.8c-1.4 5.1-1.6 10.5-2.5 15.7-.6 5.3-2.2 10.7-1.4 16 2.8-2.1 2.5-6 3.1-9.2.8-7.3 2.4-14.5 2.9-21.8q-.3-3.4-2.1-.7m16.5-.4 1.6-5.8h10.4l-5 34.9c-.4 3.8-3.2 7.5-7.2 8-3.5 1-8.2-1-8.4-5-.3-4.8 1-9.6 1.4-14.4h6.8c-.7 4.7-1.6 9.4-1.6 14.2 1.3-1 2.4-2 2.4-3.7l4-28.2zm8.8 36.6 5.9-42.4h13l-.7 5.8h-6.5l-1.5 10.6h5l-.8 6.1h-5l-2 14h6.5l-1 6zm25.2-42.2c3.8-1.8 9.9-1 10.8 3.8.5 4.8-1 9.6-1.4 14.4H241c.6-4.7 1.6-9.3 1.2-14-3.2 2-2.7 6.2-3.3 9.5-.8 7.4-2.5 14.6-2.9 22l1 1.8c3-4 2-9.5 3.3-14.2h6.5c-.7 4.7-.9 9.6-2.3 14-1.9 5-8.8 7.4-13.1 4.1-2.7-2-2.2-5.8-1.8-8.7l3.3-24c.4-3.5 2-7.2 5.3-8.7m13.9-.1 15.8-.1-.8 5.7-4.6.1-5.1 36.6h-6.6l5.1-36.6h-4.6zm17.5 4.6c1.7-5.4 9.9-7.3 13.6-3 2.4 4 .5 8.8.2 13h-6.6q1-5 1-10c-3.2 1.4-2.6 5.4-3.1 8.3-.7 5.2 7 6 7.4 10.9q0 6.8-1.8 13.5c-1.8 5.8-11 8-14.5 2.8-2-4.6.1-9.7.5-14.4h6.5c-.5 3.8-1.3 7.5-1.1 11.3 1.6.7 2.3-1 2.7-2.2.6-3 1.2-6 .8-9-1.8-3.8-7-5.3-7.2-10q-.1-5.7 1.6-11.2'/></svg></a></div>"
"</main></div>"

"<script>"
"let rateLimitDirty=false;let colorShiftDirty=false;let prev=null;let rates={underruns:0,payload:0,packets:0,bad:0,dropped:0,rxTimeouts:0,sd:0,idn:0,ether:0,ilw:0};let bufDirty=false;let mirrorDirty=false;let lastRpStatus=null;let lastSdSig=null;let currentSdPath='';let diagBase=null;let testColorMask=7;"
"function showTab(id,btn){btn=btn||document.querySelector('.nav button[data-tab='+id+']');document.querySelectorAll('.section').forEach(s=>s.classList.remove('active'));document.getElementById(id).classList.add('active');document.querySelectorAll('.nav button').forEach(b=>b.classList.remove('active'));if(btn)btn.classList.add('active');document.getElementById('crumb').innerText=btn?btn.innerText:id;if(id==='playback')loadSdFiles();if(id==='protocols')loadProtocolSettings();if(id==='network')loadNetworkSettings();}"
"function post(url){return fetch(url,{method:'POST'}).then(r=>r.text()).then(t=>{console.log(url,t);setTimeout(update,120);return t;});}"
"function refreshEngine(){return post('/api/scan/refresh');}"
"function stopAll(){return post('/api/rp/stop');}"
"function setRate(r){return post('/api/rp/set_rate?value='+encodeURIComponent(r));}"
"function setColorShiftMsg(t,kind){let e=document.getElementById('colorShiftMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function shiftReq(q,busy,done){setColorShiftMsg(busy,'ok');return fetch('/api/rp/color_shift?'+q,{method:'POST'}).then(r=>r.text().then(t=>{if(!r.ok||!t.startsWith('OK')){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}colorShiftDirty=false;setColorShiftMsg(done,'ok');setTimeout(update,120);return t;})).catch(e=>setColorShiftMsg(e.message||'Failed','err'));}"
"function applyColorShift(){let v=Number(val('colorShiftInput'));if(!Number.isInteger(v)||v<-2000||v>2000){setColorShiftMsg('Enter -2000 to 2000','err');return;}return shiftReq('value='+v,'Applying...','Applied.');}"
"function stepColorShift(d){let e=document.getElementById('colorShiftInput');let v=Math.max(-2000,Math.min(2000,(Number(e&&e.value)||0)+d));if(e)e.value=v;return shiftReq('value='+v,'Applying...','Applied.');}"
"function saveColorShift(){let v=Number(val('colorShiftInput'));if(!Number.isInteger(v)||v<-2000||v>2000){setColorShiftMsg('Enter -2000 to 2000','err');return;}let s=lastRpStatus||{};if(s.remote_running&&!s.test_stream_running){setColorShiftMsg('Stop the stream to save','err');return;}let stop=s.test_stream_running?(setColorShiftMsg('Stopping pattern...','ok'),post('/api/rp/stop_test_stream').then(()=>waitOutputStopped(3000))):Promise.resolve();return stop.then(()=>shiftReq('value='+v,'Saving...','Saved.')).then(t=>t&&shiftReq('save=1','Saving...','Saved.')).catch(e=>setColorShiftMsg(e.message||'Failed','err'));}"
"function waitOutputStopped(ms){let end=Date.now()+ms;let poll=()=>fetch('/api/scan/refresh',{method:'POST'}).then(r=>r.text()).then(t=>{if(!t.startsWith('OK'))throw new Error('DAC Engine not answering');return fetch('/api/rp/status',{cache:'no-store'});}).then(r=>r.json()).then(j=>{if(!j.remote_running)return j;if(Date.now()>end)throw new Error('Output did not stop');return new Promise(r=>setTimeout(r,200)).then(poll);});return poll();}"
"function syncColorShift(j){if(!colorShiftDirty)setValIfIdle('colorShiftInput',j.remote_color_shift_us||0);let m=document.getElementById('colorShiftMsg');if(m&&!colorShiftDirty&&m.className.indexOf('err')<0)m.innerText=(j.remote_color_shift_us||0)+' \u00b5s = '+(j.remote_color_shift_samples||0)+' samples at '+fmtKpps(j.remote_sample_rate)+(j.remote_color_shift_saved===false?' (applied, not saved)':' (saved)');}"
"function setRateLimitMsg(t,kind){let e=document.getElementById('rateLimitMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function saveRateLimit(){let lim=Number(val('rateLimitInput'));if(!Number.isInteger(lim)||lim<1000||lim>200000){setRateLimitMsg('Enter a limit between 1000 and 200000 pps','err');return;}setRateLimitMsg('Saving...','ok');return fetch('/api/rp/rate_limit?value='+lim,{method:'POST'}).then(r=>r.text().then(t=>{if(!r.ok||!t.startsWith('OK')){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}rateLimitDirty=false;setRateLimitMsg('Saved and applied.','ok');setTimeout(update,120);return t;})).catch(e=>setRateLimitMsg(e.message||'Save failed','err'));}"
"function matchRateLimitPreset(v){let s=document.getElementById('rateLimitPreset');if(!s||document.activeElement===s)return;let n=Number(v);s.value=Array.from(s.options).some(o=>o.value&&Number(o.value)===n)?String(n):'';}"
"function onRateLimitPreset(){let v=val('rateLimitPreset');if(!v)return;let e=document.getElementById('rateLimitInput');if(e)e.value=v;rateLimitDirty=true;setRateLimitMsg('Press Save limit to apply '+fmtKpps(Number(v)),'ok');}"
"function syncRateLimit(j){let lim=Number(j.rate_limit||0);document.querySelectorAll('.rateBtn').forEach(b=>{b.disabled=lim>0&&Number(b.dataset.rate)>lim;});if(!rateLimitDirty){setValIfIdle('rateLimitInput',lim||'');matchRateLimitPreset(lim);}let msg=document.getElementById('rateLimitMsg');if(msg&&!rateLimitDirty&&!msg.innerText.startsWith('Saved')&&msg.className.indexOf('err')<0){msg.innerText=lim?('Active: '+fmtKpps(lim)+(j.remote_max_rate?' (DAC Engine '+fmtKpps(j.remote_max_rate)+')':' (DAC Engine firmware has no limit of its own)')):'';}}"
"function setCustomRate(){setRate(document.getElementById('rateInput').value);}"
"function syncTestColorButtons(){['testRedBtn','testGreenBtn','testBlueBtn','testBlackBtn'].forEach(id=>{let e=document.getElementById(id);if(e)e.classList.remove('active');});let r=document.getElementById('testRedBtn');let g=document.getElementById('testGreenBtn');let b=document.getElementById('testBlueBtn');let k=document.getElementById('testBlackBtn');if((testColorMask&1)&&r)r.classList.add('active');if((testColorMask&2)&&g)g.classList.add('active');if((testColorMask&4)&&b)b.classList.add('active');if(testColorMask===0&&k)k.classList.add('active');}"
"function sendTestColor(){syncTestColorButtons();return post('/api/rp/test_color?mask='+encodeURIComponent(testColorMask));}"
"function toggleTestColor(bit){testColorMask^=bit;sendTestColor();}"
"function setTestBlack(){testColorMask=0;sendTestColor();}"
"function onMirrorInput(){mirrorDirty=true;setMirrorMsg('Unsaved changes.','');}"
"function saveMirrorSettings(){let p=new URLSearchParams();p.set('mirror_x',checked('mirrorX')?'1':'0');p.set('mirror_y',checked('mirrorY')?'1':'0');setMirrorMsg('Saving to DAC Engine flash...','ok');return fetch('/api/rp/mirror_config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok||!t.startsWith('OK')){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}mirrorDirty=false;setMirrorMsg('Saved and applied.','ok');setTimeout(update,120);return t;})).catch(e=>setMirrorMsg(e.message||'Save failed','err'));}"
"function onBufInput(){bufDirty=true;document.getElementById('bufCancelBtn').style.display='';setBufMsg('Unsaved changes.','');}"
"function cancelBufEdit(){bufDirty=false;document.getElementById('bufCancelBtn').style.display='none';setBufMsg('Applies immediately and saves to RP flash.','');if(lastRpStatus)forceSyncBufInputs(lastRpStatus);}"
"function defaultHigh(size){return size>2048?size-2048:Math.max(256,size-256);}"
"function defaultLow(high){return high>2048?high-2048:Math.max(0,high-256);}"
"function forceSyncBufInputs(j){let size=Number(j.remote_buffer_size_samples||0);let high=Number(j.remote_buffer_high_water_samples||0);let low=Number(j.remote_buffer_low_water_samples||0);if(size&&!high)high=defaultHigh(size);setVal('bufSizeInput',size||'');setVal('bufHighInput',high||'');setVal('bufLowInput',low);}"
"function saveBufferSettings(){let size=Number(val('bufSizeInput'));let high=Number(val('bufHighInput'));let low=Number(val('bufLowInput'));let max=lastRpStatus?Number(lastRpStatus.remote_buffer_max_samples||0):0;if(!Number.isInteger(size)||!Number.isInteger(high)||!Number.isInteger(low)||size<512||high<256||low<0){setBufMsg('Enter valid whole-number buffer limits','err');return;}if(max&&size>max){setBufMsg('Buffer size is above the RP maximum','err');return;}if(high>size-256){setBufMsg('High-water limit must leave at least 256 samples of headroom','err');return;}if(low>high-256){setBufMsg('Low-water limit must be at least 256 samples below high-water','err');return;}let p=new URLSearchParams();p.set('size',size);p.set('high_water',high);p.set('low_water',low);setBufMsg('Saving to RP flash...','ok');return fetch('/api/rp/buffer_config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok||!t.startsWith('OK')){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}setBufMsg('Saved and applied.','ok');bufDirty=false;document.getElementById('bufCancelBtn').style.display='none';setTimeout(update,120);return t;})).catch(e=>setBufMsg(e.message||'Save failed','err'));}"
"function startIldaProject(path,relativePath){let scale=Number(val('ildaScale'))||100;return post('/api/ilda/start?path='+encodeURIComponent(path)+'&scale='+scale).then(result=>{if(result.trim()==='OK'){activePreviewProjectPath=relativePath;syncPreviewProjectControls({ilda_running:true});if(relativePath!==previewPath){selectPreviewRow(relativePath);loadIldaPreview(relativePath,0);}setTimeout(update,120);}else if(relativePath!==previewPath){selectPreviewRow(relativePath);loadIldaPreview(relativePath,0);}else{setText('previewVerdict','Play failed: '+result);setClass('previewVerdict','previewVerdict err');}});}"
"let lastPlayerFile='';function followPlayer(j){let file=j.ilda_running?(j.ilda_file||''):'';if(file&&file!==lastPlayerFile&&file!==previewPath){selectPreviewRow(file);loadIldaPreview(file,0);}lastPlayerFile=file;}"
"function loadFolderPrefs(){try{let p=JSON.parse(localStorage.getItem('folderPlay')||'{}');if(p.hold)setVal('folderHold',p.hold);if(p.plays)setVal('folderPlays',p.plays);let r=document.getElementById('folderRecursive');if(r)r.checked=!!p.recursive;}catch(e){}}"
"function playSdFolder(){let hold=Number(val('folderHold'));let plays=Number(val('folderPlays'));let r=document.getElementById('folderRecursive');let recursive=!!(r&&r.checked);if(!Number.isInteger(hold)||hold<1||hold>3600){setText('folderPlayMsg','Hold must be 1-3600 seconds');return;}if(!Number.isInteger(plays)||plays<1||plays>1000){setText('folderPlayMsg','Animation plays must be 1-1000');return;}try{localStorage.setItem('folderPlay',JSON.stringify({hold:hold,plays:plays,recursive:recursive}));}catch(e){}let scale=Number(val('ildaScale'))||100;setText('folderPlayMsg','Starting...');return post('/api/ilda/play_folder?path='+encodeURIComponent(currentSdPath)+'&hold='+hold+'&plays='+plays+'&recursive='+(recursive?1:0)+'&scale='+scale).then(result=>{setText('folderPlayMsg',result.trim()==='OK'?'':result.trim()==='ERR ESP_ERR_NOT_FOUND'?'No playable ILDA files in this folder':result);setTimeout(update,120);});}"
"function selectPreviewRow(path){document.querySelectorAll('.sdTable tbody tr').forEach(row=>row.classList.toggle('previewSelected',row.dataset.path===path));}"
"function playIldaPath(p){let relativePath=p.startsWith('/sdcard/')?p.substring(8):p;startIldaProject(p,relativePath);}"
"function startPreviewProject(){if(!previewPath)return;startIldaProject('/sdcard/'+previewPath,previewPath);}"
"function stopPreviewProject(){return post('/api/ilda/stop').then(result=>{if(result.trim()==='OK'){activePreviewProjectPath='';syncPreviewProjectControls({ilda_running:false});setText('previewVerdict','Projection stopped.');setClass('previewVerdict','previewVerdict ok');}else{setText('previewVerdict',result);setClass('previewVerdict','previewVerdict err');}setTimeout(update,120);});}"
"let previewPath='';let previewFrame=0;let previewFrameCount=0;let previewFramePoints=0;let previewRequestId=0;let previewPlaying=false;let previewTimer=0;let activePreviewProjectPath='';let previewScaleTimer=0;let previewLoopReturnSamples=0;let previewAbort=null;"
"let previewPoints=[];function onIldaScaleChange(){let scale=Number(val('ildaScale'))||100;let output=document.getElementById('ildaScaleValue');if(output)output.value=scale+'%';drawIldaPreview(previewPoints);if(activePreviewProjectPath===previewPath){if(previewScaleTimer)clearTimeout(previewScaleTimer);previewScaleTimer=setTimeout(()=>{post('/api/ilda/scale?value='+scale).then(result=>{if(result.trim()!=='OK'){setText('previewVerdict',result);setClass('previewVerdict','previewVerdict err');}});},100);}}"
"function syncPreviewProjectControls(status){let running=!!(status&&status.ilda_running);if(!running)activePreviewProjectPath='';else if(status.ilda_file)activePreviewProjectPath=status.ilda_file;let sameFile=running&&activePreviewProjectPath===previewPath;let start=document.getElementById('previewProjectBtn');let stop=document.getElementById('previewStopBtn');if(start){start.disabled=!previewPath||sameFile;start.textContent=sameFile?'Projecting':'Project';}if(stop)stop.disabled=!running;}"
"function setPreviewPlayback(playing){previewPlaying=playing;if(previewTimer)clearTimeout(previewTimer);previewTimer=0;let button=document.getElementById('previewPlayBtn');if(button){button.textContent=playing?'Pause':'Play';button.setAttribute('aria-label',playing?'Pause preview':'Play preview');button.setAttribute('aria-pressed',playing?'true':'false');}}"
"function drawIldaPreview(points){if(points)previewPoints=points;let canvas=document.getElementById('ildaPreviewCanvas');if(!canvas)return;let ctx=canvas.getContext('2d');let w=canvas.width,h=canvas.height;ctx.clearRect(0,0,w,h);ctx.fillStyle='#080b0d';ctx.fillRect(0,0,w,h);let margin=16,size=Math.min(w,h)-margin*2,left=(w-size)/2,top=(h-size)/2,scale=size/65535;ctx.strokeStyle='#202a30';ctx.lineWidth=1;ctx.strokeRect(left,top,size,size);for(let i=1;i<4;i++){let x=left+size*i/4,y=top+size*i/4;ctx.beginPath();ctx.moveTo(x,top);ctx.lineTo(x,top+size);ctx.moveTo(left,y);ctx.lineTo(left+size,y);ctx.stroke();}ctx.strokeStyle='#39464e';ctx.beginPath();ctx.moveTo(left+size/2,top);ctx.lineTo(left+size/2,top+size);ctx.moveTo(left,top+size/2);ctx.lineTo(left+size,top+size/2);ctx.stroke();let xyScale=(Number(val('ildaScale'))||100)/100,previous=null;ctx.lineWidth=1.5;ctx.lineCap='round';ctx.lineJoin='round';previewPoints.forEach(p=>{let point={x:left+(32768+p[0]*xyScale)*scale,y:top+(32767-p[1]*xyScale)*scale,blanked:!!p[5],r:p[2],g:p[3],b:p[4]};if(previous&&!point.blanked){ctx.strokeStyle='rgb('+point.r+','+point.g+','+point.b+')';ctx.beginPath();ctx.moveTo(previous.x,previous.y);ctx.lineTo(point.x,point.y);ctx.stroke();}previous=point;});}"
"function val(id){let e=document.getElementById(id);return e?e.value:'';}"
"function setVal(id,v){let e=document.getElementById(id);if(e)e.value=(v===0||v)?v:'';}"
"function setValIfIdle(id,v){let e=document.getElementById(id);if(e&&document.activeElement!==e)e.value=(v===0||v)?v:'';}"
"function checked(id){let e=document.getElementById(id);return !!(e&&e.checked);}"
"function setChecked(id,v){let e=document.getElementById(id);if(e)e.checked=!!v;}"
"function updateIpFields(){let e=document.getElementById('netIpFields');if(e)e.style.display=val('netIpMode')==='static'?'block':'none';}"
"function setNetMsg(t,kind){let e=document.getElementById('netSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setProtoMsg(t,kind){let e=document.getElementById('protoSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setSystemMsg(t,kind){let e=document.getElementById('systemMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setBufMsg(t,kind){let e=document.getElementById('bufSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setMirrorMsg(t,kind){let e=document.getElementById('mirrorSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function loadProtocolSettings(){return fetch('/api/protocols/config').then(r=>r.json()).then(j=>{setChecked('protoSdEnabled',true);setChecked('protoIdnEnabled',j.idn_enabled);setChecked('protoEtherEnabled',j.etherdream_enabled);setChecked('protoIlwEnabled',j.ilw_enabled);setText('protoIdnTransport',j.idn_transport||'UDP');setText('protoIdnPort',j.idn_udp_port);setText('protoEtherTcp',j.etherdream_tcp_port);setText('protoEtherUdp',j.etherdream_udp_port);setText('protoIlwTransport',j.ilw_transport||'UDP');setText('protoIlwPort',j.ilw_udp_port);let a=j.active||{};setText('protoIdnActive',a.idn_enabled?'Enabled':'Disabled');setText('protoEtherActive',a.etherdream_enabled?'Enabled':'Disabled');setText('protoIlwActive',a.ilw_enabled?'Enabled':'Disabled');setClass('protoIdnActive',a.idn_enabled?'v status-ok':'v status-warn');setClass('protoEtherActive',a.etherdream_enabled?'v status-ok':'v status-warn');setClass('protoIlwActive',a.ilw_enabled?'v status-ok':'v status-warn');setProtoMsg(j.reboot_required?'Saved settings pending restart':'Settings are saved to flash and applied on boot.',j.reboot_required?'ok':'');}).catch(e=>setProtoMsg('Protocol settings load failed','err'));}"
"function saveProtocolSettings(){let p=new URLSearchParams();p.set('sd_ilda_enabled','1');p.set('idn_enabled',checked('protoIdnEnabled')?'1':'0');p.set('etherdream_enabled',checked('protoEtherEnabled')?'1':'0');p.set('ilw_enabled',checked('protoIlwEnabled')?'1':'0');return fetch('/api/protocols/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}setProtoMsg('Saved. Restart device to apply.','ok');loadProtocolSettings();return t;})).catch(e=>setProtoMsg(e.message||'Save failed','err'));}"
"function loadNetworkSettings(){return fetch('/api/network/config').then(r=>r.json()).then(j=>{setChecked('netApEnabled',j.wifi_ap_enabled);setVal('netApSsid',j.wifi_ap_ssid);setVal('netApPass','');setChecked('netStaEnabled',j.wifi_sta_enabled);setVal('netStaSsid',j.wifi_sta_ssid);setVal('netStaPass','');setVal('netHostname',j.hostname);setVal('netIpMode',j.ip_mode);setVal('netStaticIp',j.static_ip);setVal('netStaticMask',j.static_netmask);setVal('netStaticGateway',j.static_gateway);setVal('netStaticDns',j.static_dns);updateIpFields();let s=j.status||{};setText('netEthState',s.eth_link?(s.eth_has_ip?'Ready':'Link only'):'Disconnected');setText('netEthIp',s.eth_has_ip?s.eth_ip:'--');setText('netApIp',s.wifi_ap_enabled?s.wifi_ap_ip:'Disabled');setText('netStaState',!j.wifi_sta_enabled?'Disabled':(s.wifi_sta_connected?'Connected':'Connecting'));setText('netStaIp',s.wifi_sta_connected?s.wifi_sta_ip:'--');setNetMsg(j.reboot_required?'Saved settings pending restart':'Settings are saved to flash and applied on boot.',j.reboot_required?'ok':'');}).catch(e=>setNetMsg('Network settings load failed','err'));}"
"function saveNetworkSettings(){let p=new URLSearchParams();let apPass=val('netApPass');let staPass=val('netStaPass');p.set('ap_enabled',checked('netApEnabled')?'1':'0');p.set('ap_ssid',val('netApSsid'));if(apPass)p.set('ap_password',apPass);p.set('sta_enabled',checked('netStaEnabled')?'1':'0');p.set('sta_ssid',val('netStaSsid'));if(staPass)p.set('sta_password',staPass);p.set('hostname',val('netHostname'));p.set('ip_mode',val('netIpMode'));p.set('static_ip',val('netStaticIp'));p.set('static_netmask',val('netStaticMask'));p.set('static_gateway',val('netStaticGateway'));p.set('static_dns',val('netStaticDns'));return fetch('/api/network/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}setNetMsg('Saved. Restart device to apply.','ok');loadNetworkSettings();return t;})).catch(e=>setNetMsg(e.message||'Save failed','err'));}"
"function restartDevice(){if(!confirm('Restart device now?'))return;fetch('/api/system/reboot',{method:'POST'}).then(()=>{setNetMsg('Restarting...','ok');setProtoMsg('Restarting...','ok');setSystemMsg('Restarting...','ok');}).catch(e=>{setNetMsg('Restart failed','err');setProtoMsg('Restart failed','err');setSystemMsg('Restart failed','err');});}"
"function factoryReset(){if(!confirm('Restore default settings and restart now?'))return;fetch('/api/system/factory_reset',{method:'POST'}).then(r=>r.text().then(t=>{if(!r.ok)throw new Error(t.startsWith('ERR ')?t.substring(4):t);setSystemMsg('Factory defaults restored. Restarting...','ok');setNetMsg('Factory defaults restored. Restarting...','ok');setProtoMsg('Factory defaults restored. Restarting...','ok');})).catch(e=>setSystemMsg(e.message||'Factory reset failed','err'));}"
"function fmtBytes(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB';}"
"function fmtKpps(pps){return (pps/1000).toFixed(0)+' kpps';}"
"function fmtRate(v){return v>=1000?(v/1000).toFixed(1)+' k/s':v.toFixed(0);}"
"function fmtBuffer(j){return j.remote_buffer_size_samples?j.remote_buffer_fill_samples+'/'+j.remote_buffer_size_samples:j.remote_buffer_fill_samples;}"
"function goSdParent(){let i=currentSdPath.lastIndexOf('/');loadSdFiles(i<0?'':currentSdPath.slice(0,i));}"
"function loadSdFiles(path){if(path===undefined)path=currentSdPath;let url='/api/sd/list';if(path)url+='?path='+encodeURIComponent(path);fetch(url).then(r=>r.json()).then(j=>{let body=document.getElementById('fileRows');if(!body)return;body.replaceChildren();currentSdPath=j.current||'';setText('sdCurrentPath','/'+currentSdPath);let up=document.getElementById('sdUpBtn');if(up)up.disabled=!currentSdPath;if(!j.files)return;j.files.forEach(f=>{let tr=document.createElement('tr');tr.dataset.path=f.browse_path||'';if(f.browse_path===previewPath)tr.classList.add('previewSelected');let name=document.createElement('td');name.textContent=f.name;tr.appendChild(name);let size=document.createElement('td');size.className='sizeCell';size.textContent=f.is_dir?'--':fmtBytes(f.size);tr.appendChild(size);let action=document.createElement('td');action.className='actionCell';if(f.is_dir){let open=document.createElement('button');open.textContent='Open';open.onclick=()=>loadSdFiles(f.browse_path);action.appendChild(open);}else if(f.name.toLowerCase().endsWith('.ild')){let preview=document.createElement('button');preview.className='previewBtn';preview.textContent='Preview';preview.onclick=()=>{selectPreviewRow(f.browse_path);loadIldaPreview(f.browse_path,0);};action.appendChild(preview);let play=document.createElement('button');play.className='green playBtn';play.textContent='Play';play.onclick=()=>playIldaPath(f.path);action.appendChild(play);}else{action.textContent='--';}tr.appendChild(action);body.appendChild(tr);});}).catch(e=>{let body=document.getElementById('fileRows');if(body)body.replaceChildren();});}"
"function syncSdFiles(j){let sig=[j.sd_inserted?1:0,j.sd_mounted?1:0,j.sd_file_count||0].join(':');if(sig!==lastSdSig){lastSdSig=sig;loadSdFiles();}}"
"function syncProtocolRates(){setText('protoRateSd',fmtRate(rates.sd));setText('protoRateIdn',fmtRate(rates.idn));setText('protoRateEther',fmtRate(rates.ether));setText('protoRateIlw',fmtRate(rates.ilw));setText('protoStateSd',rates.sd>0?'Active':'Idle');setText('protoStateIdn',rates.idn>0?'Active':'Idle');setText('protoStateEther',rates.ether>0?'Active':'Idle');setText('protoStateIlw',rates.ilw>0?'Active':'Idle');setClass('protoStateSd',rates.sd>0?'v status-ok':'muted');setClass('protoStateIdn',rates.idn>0?'v status-ok':'muted');setClass('protoStateEther',rates.ether>0?'v status-ok':'muted');setClass('protoStateIlw',rates.ilw>0?'v status-ok':'muted');}"
"function countFromBase(j,k){let b=diagBase?Number(diagBase[k]||0):0;return Math.max(0,Number(j[k]||0)-b);}"
"function syncDiagnosticTotals(j){setText('diagPackets',countFromBase(j,'remote_stream_packets'));setText('diagPayloadErrors',countFromBase(j,'remote_payload_errors'));setText('diagUnderruns',countFromBase(j,'remote_underruns'));setText('diagBadPackets',countFromBase(j,'remote_stream_bad_packets'));setText('diagDroppedPackets',countFromBase(j,'remote_stream_dropped_packets'));setText('diagRxTimeouts',countFromBase(j,'remote_stream_rx_timeouts'));}"
"function resetDiagnosticCounters(){if(!lastRpStatus)return;diagBase={remote_stream_packets:Number(lastRpStatus.remote_stream_packets||0),remote_payload_errors:Number(lastRpStatus.remote_payload_errors||0),remote_underruns:Number(lastRpStatus.remote_underruns||0),remote_stream_bad_packets:Number(lastRpStatus.remote_stream_bad_packets||0),remote_stream_dropped_packets:Number(lastRpStatus.remote_stream_dropped_packets||0),remote_stream_rx_timeouts:Number(lastRpStatus.remote_stream_rx_timeouts||0)};syncDiagnosticTotals(lastRpStatus);}"
"function calcRates(j){let now=Date.now();if(prev){let dt=(now-prev.t)/1000;if(dt>0){rates.underruns=Math.max(0,(j.remote_underruns-prev.underruns)/dt);rates.payload=Math.max(0,(j.remote_payload_errors-prev.payload)/dt);rates.packets=Math.max(0,(j.remote_stream_packets-prev.packets)/dt);rates.bad=Math.max(0,(j.remote_stream_bad_packets-prev.bad)/dt);rates.dropped=Math.max(0,(j.remote_stream_dropped_packets-prev.dropped)/dt);rates.rxTimeouts=Math.max(0,(j.remote_stream_rx_timeouts-prev.rxTimeouts)/dt);rates.sd=Math.max(0,((j.stream_sd_ilda_samples||0)-prev.sd)/dt);rates.idn=Math.max(0,((j.stream_idn_samples||0)-prev.idn)/dt);rates.ether=Math.max(0,((j.stream_etherdream_samples||0)-prev.ether)/dt);rates.ilw=Math.max(0,((j.stream_ilw_samples||0)-prev.ilw)/dt);}}prev={t:now,underruns:j.remote_underruns,payload:j.remote_payload_errors,packets:j.remote_stream_packets,bad:j.remote_stream_bad_packets,dropped:j.remote_stream_dropped_packets,rxTimeouts:j.remote_stream_rx_timeouts,sd:j.stream_sd_ilda_samples||0,idn:j.stream_idn_samples||0,ether:j.stream_etherdream_samples||0,ilw:j.stream_ilw_samples||0};}"
"function resetUiCounters(){prev=null;rates={underruns:0,payload:0,packets:0,bad:0,dropped:0,rxTimeouts:0,sd:0,idn:0,ether:0,ilw:0};syncProtocolRates();}"
"function setText(id,v){let e=document.getElementById(id);if(e)e.innerText=v;}"
"function setClass(id,v){let e=document.getElementById(id);if(e)e.className=v;}"
"function setButtonState(running){}"
"function diagnosticStatus(j){return {product:j.product,esp_fw:j.esp_fw,rp_fw:j.rp_fw,dac_engine_ready:!!j.initialized,dac_running:!!j.remote_running,sample_rate:j.remote_sample_rate,buffer_fill:j.remote_buffer_fill_samples,buffer_size:j.remote_buffer_size_samples,buffer_high_water:j.remote_buffer_high_water_samples,buffer_low_water:j.remote_buffer_low_water_samples,buffer_max:j.remote_buffer_max_samples,mirror_x:!!j.remote_mirror_x,mirror_y:!!j.remote_mirror_y,rate_limit:j.rate_limit,rp_max_rate:j.remote_max_rate,max_lit_step:j.remote_max_lit_step,lit_big_steps:j.remote_lit_big_steps,blank_big_steps:j.remote_blank_big_steps,idn_dropped:j.stream_idn_dropped,etherdream_dropped:j.stream_etherdream_dropped,underruns:j.remote_underruns,payload_errors:j.remote_payload_errors,stream_packets:j.remote_stream_packets,bad_packets:j.remote_stream_bad_packets,dropped_packets:j.remote_stream_dropped_packets,rx_timeouts:j.remote_stream_rx_timeouts,sd_ilda_samples:j.stream_sd_ilda_samples,idn_samples:j.stream_idn_samples,etherdream_samples:j.stream_etherdream_samples,ilw_samples:j.stream_ilw_samples,sd_ilda_running:!!j.ilda_running,test_pattern_running:!!j.test_stream_running};}"
"function syncMirrorFields(j){if(!mirrorDirty){setChecked('mirrorX',!!j.remote_mirror_x);setChecked('mirrorY',!!j.remote_mirror_y);}}"
"function syncBufferFields(j){let rpMax=Number(j.remote_buffer_max_samples||0);let rpSize=Number(j.remote_buffer_size_samples||0);let rpHigh=Number(j.remote_buffer_high_water_samples||0);let rpLow=Number(j.remote_buffer_low_water_samples||0);let sizeLimit=rpMax||rpSize;let highLimit=rpSize>256?rpSize-256:0;if(rpSize&&!rpHigh)rpHigh=defaultHigh(rpSize);let lowLimit=rpHigh>256?rpHigh-256:0;setText('bufSizeInfo',rpSize?(rpSize+' / '+sizeLimit):'--');setText('bufHighInfo',rpHigh?(rpHigh+' / '+highLimit):'--');setText('bufLowInfo',rpHigh?(rpLow+' / '+lowLimit):'--');if(!bufDirty){setValIfIdle('bufSizeInput',rpSize||'');setValIfIdle('bufHighInput',rpHigh||'');setValIfIdle('bufLowInput',rpLow);}}"
"function updateBufferMeter(j){let size=Number(j.remote_buffer_size_samples||0);let fill=Number(j.remote_buffer_fill_samples||0);let low=Number(j.remote_buffer_low_water_samples||0);let high=Number(j.remote_buffer_high_water_samples||0);let lf=size?Math.max(0,Math.min(100,low*100/size)):0;let hf=size?Math.max(lf,Math.min(100,high*100/size)):0;let ff=size?Math.max(0,Math.min(100,fill*100/size)):0;let f=document.getElementById('topBufferFill');let lm=document.getElementById('topBufLowMark');let hm=document.getElementById('topBufHighMark');if(f){f.style.width=ff+'%';f.style.background=!size?'rgba(74,163,255,.35)':fill>=high?'rgba(217,155,40,.55)':fill<low?'rgba(29,191,115,.5)':'rgba(74,163,255,.45)';}if(lm)lm.style.left=lf+'%';if(hm)hm.style.left=hf+'%';}"
"function render(j){lastRpStatus=j;calcRates(j);setButtonState(j.remote_running);syncBufferFields(j);syncMirrorFields(j);syncRateLimit(j);syncColorShift(j);syncSdFiles(j);syncProtocolRates();syncDiagnosticTotals(j);updateBufferMeter(j);let dacEngineOk=!!j.initialized;let dacRun=!!j.remote_running;let bufferText=fmtBuffer(j);if(j.product)document.title=j.product;setText('brandName',j.product);setText('aboutEspFw',j.esp_fw);setText('aboutRpFw',j.rp_fw);setText('topRate',fmtKpps(j.remote_sample_rate));setText('topFill',bufferText);setClass('topConn',dacEngineOk?'topItem ok':'topItem bad');setClass('topState',dacRun?'topItem ok':'topItem bad');setText('dashUnderrunRate',rates.underruns.toFixed(1));setText('dashPayloadRate',rates.payload.toFixed(1));setClass('dashUnderrunRate',rates.underruns>0?'v status-warn':'v status-ok');setClass('dashPayloadRate',rates.payload>0?'v status-bad':'v status-ok');document.getElementById('status').innerText=JSON.stringify(diagnosticStatus(j),null,2);}"
"function update(){fetch('/api/rp/status',{cache:'no-store'}).then(r=>r.json()).then(j=>{render(j);followPlayer(j);syncPreviewProjectControls(j);}).catch(e=>{setClass('topConn','topItem bad');});}"
"let previewFrames=[];"
"function showPreviewFrame(frame){if(frame<0||frame>=previewFrameCount)return;previewFrame=frame;let current=previewFrames[frame];previewFramePoints=Number(current.point_count)||0;setText('previewFrameLabel','Frame '+(frame+1)+' / '+previewFrameCount);setText('previewSelectedPoints',previewFramePoints.toLocaleString());document.getElementById('previewPrevBtn').disabled=frame===0;document.getElementById('previewNextBtn').disabled=frame+1>=previewFrameCount;document.getElementById('previewPlayBtn').disabled=previewFrameCount<2;drawIldaPreview(current.points||[]);}"
"function previewFrameDurationMs(points,frame){let rate=Number(lastRpStatus&&lastRpStatus.remote_sample_rate)||30000;let samples=Number(points)||0;if(frame===previewFrameCount-1)samples+=previewLoopReturnSamples;return samples*1000/rate;}"
"function schedulePreviewFrame(frame,requestId){if(!previewPlaying||previewFrameCount<2)return;let delay=previewFrameDurationMs(previewFrames[frame].point_count,frame);previewTimer=setTimeout(()=>{if(!previewPlaying||requestId!==previewRequestId)return;let next=(frame+1)%previewFrameCount;showPreviewFrame(next);schedulePreviewFrame(next,requestId);},delay);}"
"function togglePreviewPlayback(){if(previewPlaying){setPreviewPlayback(false);return;}if(!previewPath||previewFrames.length<2)return;setPreviewPlayback(true);schedulePreviewFrame(previewFrame,previewRequestId);}"
"function stepPreviewFrame(step){let next=previewFrame+step;if(next<0||next>=previewFrameCount)return;if(previewTimer)clearTimeout(previewTimer);previewTimer=0;showPreviewFrame(next);if(previewPlaying)schedulePreviewFrame(next,previewRequestId);}"
"function loadIldaPreview(path,frame){setPreviewPlayback(true);previewPath=path;let requestedFrame=frame||0;let requestId=++previewRequestId;let card=document.getElementById('ildaPreviewCard');card.style.display='block';setText('ildaPreviewTitle',path.split('/').pop()+' preview');setText('previewVerdict','Loading animation...');setClass('previewVerdict','previewVerdict muted');setText('previewFrameLabel','Loading');setText('previewFileSize','--');setText('previewFrameCount','--');setText('previewTotalPoints','--');setText('previewMaxFramePoints','--');setText('previewSelectedPoints','--');setText('previewFormats','--');previewFrames=[];previewFrameCount=0;drawIldaPreview([]);if(previewAbort)previewAbort.abort();previewAbort=new AbortController();let url='/api/ilda/preview?path='+encodeURIComponent(path)+'&frame='+requestedFrame+'&all=1';fetch(url,{cache:'no-store',signal:previewAbort.signal}).then(r=>r.text().then(t=>{if(!r.ok)throw new Error(t||'Preview request failed');return JSON.parse(t);})).then(j=>{if(requestId!==previewRequestId)return;if(!Array.isArray(j.frames)||j.frames.length!==j.frame_count)throw new Error('Animated preview data is incomplete');previewFrames=j.frames;previewFrameCount=previewFrames.length;previewLoopReturnSamples=Number(j.loop_return_samples)||0;setText('previewFileSize',fmtBytes(j.file_size));setText('previewFrameCount',j.frame_count.toLocaleString());setText('previewTotalPoints',j.total_points.toLocaleString());setText('previewMaxFramePoints',j.max_frame_points.toLocaleString());let formats=[];[0,1,4,5].forEach(f=>{if(j.format_mask&(1<<f))formats.push('Format '+f);});setText('previewFormats',formats.join(', '));setText('previewVerdict',j.within_sample_limit?'Within the player sample-count limit ('+Number(j.max_samples).toLocaleString()+')':'Exceeds the player sample-count limit ('+Number(j.max_samples).toLocaleString()+')');setClass('previewVerdict','previewVerdict '+(j.within_sample_limit?'ok':'warn'));showPreviewFrame(Math.min(requestedFrame,previewFrameCount-1));if(previewPlaying)schedulePreviewFrame(previewFrame,requestId);}).catch(e=>{if(requestId!==previewRequestId)return;setPreviewPlayback(false);previewFrames=[];previewFrameCount=0;setText('previewFrameLabel','Unavailable');setText('previewVerdict',e.message||'Unable to preview ILDA file');setClass('previewVerdict','previewVerdict err');document.getElementById('previewPrevBtn').disabled=true;document.getElementById('previewNextBtn').disabled=true;document.getElementById('previewPlayBtn').disabled=true;drawIldaPreview([]);});}"
"function boot(){loadFolderPrefs();syncTestColorButtons();loadNetworkSettings();loadProtocolSettings();refreshEngine().finally(()=>{update();});}"
"setInterval(update,2000);boot();"
"</script>"
"</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_portal_get_handler(httpd_req_t *req)
{
    if (strncmp(req->uri, "/api/", 5) == 0) {
        set_common_response_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Not found");
    }

    set_common_response_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Redirecting to ILDAWaveX16 setup");
}

/* ===================== REGISTRATION ===================== */

static esp_err_t register_handler(const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t uri_config = {
        .uri = path,
        .method = method,
        .handler = handler,
        .user_ctx = NULL
    };

    return httpd_register_uri_handler(s_server, &uri_config);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 44;
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.backlog_conn = 4;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    config.stack_size = WEB_SERVER_TASK_STACK_BYTES;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting web server");

    if (!s_preview_queue) {
        s_preview_queue = xQueueCreate(WEB_PREVIEW_QUEUE_LENGTH, sizeof(httpd_req_t *));
        if (!s_preview_queue) {
            return ESP_ERR_NO_MEM;
        }

        BaseType_t ok = xTaskCreate(
            ilda_preview_task,
            "ilda_preview",
            WEB_PREVIEW_TASK_STACK_BYTES,
            NULL,
            tskIDLE_PRIORITY + 1,
            NULL
        );

        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to start ILDA preview task");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(register_handler("/", HTTP_GET, root_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/sd/list", HTTP_GET, sd_list_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/ilda/preview", HTTP_GET, ilda_preview_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/network/config", HTTP_GET, network_config_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/network/config", HTTP_POST, network_config_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/protocols/config", HTTP_GET, protocol_config_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/protocols/config", HTTP_POST, protocol_config_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/system/reboot", HTTP_POST, system_reboot_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/system/factory_reset", HTTP_POST, system_factory_reset_post_handler));

    ESP_ERROR_CHECK(register_handler("/api/rp/status", HTTP_GET, rp_status_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/scan/refresh", HTTP_POST, rp_refresh_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/ping", HTTP_POST, rp_ping_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/start", HTTP_POST, rp_start_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/stop", HTTP_POST, rp_stop_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/clear", HTTP_POST, rp_clear_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/start_test_stream", HTTP_POST, rp_start_stream_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/test_color", HTTP_POST, rp_test_color_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/stop_test_stream", HTTP_POST, rp_stop_stream_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/set_rate", HTTP_POST, rp_set_rate_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/rate_limit", HTTP_POST, rp_rate_limit_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/color_shift", HTTP_POST, rp_color_shift_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/buffer_config", HTTP_POST, rp_buffer_config_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/mirror_config", HTTP_POST, rp_mirror_config_post_handler));

    ESP_ERROR_CHECK(register_handler("/api/ilda/start", HTTP_POST, ilda_start_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/ilda/scale", HTTP_POST, ilda_scale_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/ilda/play_folder", HTTP_POST, ilda_folder_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/ilda/stop", HTTP_POST, ilda_stop_post_handler));
    ESP_ERROR_CHECK(register_handler("/*", HTTP_GET, captive_portal_get_handler));

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            dac_engine_refresh_task,
            "dac_refresh",
            4096,
            NULL,
            2,
            &s_refresh_task,
            0
        );

        if (ok != pdPASS) {
            ESP_LOGW(TAG, "Failed to start scan refresh task");
        }
    }

    return ESP_OK;
}
