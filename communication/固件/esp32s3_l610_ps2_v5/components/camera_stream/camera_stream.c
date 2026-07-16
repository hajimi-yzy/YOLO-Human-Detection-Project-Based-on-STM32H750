#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>

#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "camera_stream.h"


static const char *TAG = "CAMERA_UDP";
static char s_uplink_ifname[IFNAMSIZ];

#if CONFIG_VIDEO_CAMERA_FRAME_SIZE_QVGA
#define CAMERA_FRAME_SIZE       FRAMESIZE_QVGA
#define CAMERA_FRAME_SIZE_NAME  "QVGA 320x240"
#else
#define CAMERA_FRAME_SIZE       FRAMESIZE_VGA
#define CAMERA_FRAME_SIZE_NAME  "VGA 640x480"
#endif
#define CAMERA_JPEG_QUALITY     CONFIG_VIDEO_CAMERA_JPEG_QUALITY
#define MAX_JPEG_SIZE           (256U * 1024U)
#define UDP_PAYLOAD_SIZE        CONFIG_VIDEO_UDP_PAYLOAD_SIZE
#define ESJP_HEADER_SIZE        38U
#define ESJP_VERSION            CONFIG_VIDEO_UDP_PROTOCOL_VERSION
#define NETWORK_READY_BIT       BIT0
#define SEND_FAILURE_LIMIT      3
#define LOCAL_MJPEG_BOUNDARY    "frame"
#define LOCAL_MJPEG_TYPE        "multipart/x-mixed-replace;boundary=" LOCAL_MJPEG_BOUNDARY
#define LOCAL_MJPEG_PREFIX      "--" LOCAL_MJPEG_BOUNDARY "\r\n"
#define LOCAL_MJPEG_FRAME_WAIT_MS 20

#if CONFIG_VIDEO_UDP_PROTOCOL_VERSION == 1
_Static_assert(CONFIG_VIDEO_UDP_PAYLOAD_SIZE == 1200,
               "ESJP v1 requires a 1200-byte chunk payload");
#elif CONFIG_VIDEO_UDP_PROTOCOL_VERSION == 2
_Static_assert(CONFIG_VIDEO_UDP_PAYLOAD_SIZE == 7200,
               "ESJP v2 requires a 7200-byte chunk payload");
#elif CONFIG_VIDEO_UDP_PROTOCOL_VERSION == 3
_Static_assert(CONFIG_VIDEO_UDP_PAYLOAD_SIZE == 1400,
               "ESJP v3 requires a 1400-byte chunk payload");
#else
#error "Unsupported ESJP protocol version"
#endif

/* ESP32-S3-CAM / OV5640 wiring verified from the supplied schematic. */
#define CAM_PIN_PWDN       -1
#define CAM_PIN_RESET      -1
#define CAM_PIN_XCLK       15
#define CAM_PIN_SIOD        4
#define CAM_PIN_SIOC        5
#define CAM_PIN_D7         16
#define CAM_PIN_D6         17
#define CAM_PIN_D5         18
#define CAM_PIN_D4         12
#define CAM_PIN_D3         10
#define CAM_PIN_D2          8
#define CAM_PIN_D1          9
#define CAM_PIN_D0         11
#define CAM_PIN_VSYNC       6
#define CAM_PIN_HREF        7
#define CAM_PIN_PCLK       13

static EventGroupHandle_t s_network_events;
static bool s_camera_initialized;
static bool s_task_started;
static SemaphoreHandle_t s_local_frame_lock;
static uint8_t *s_local_frame_buffer;
static size_t s_local_frame_length;
static uint32_t s_local_frame_sequence;
static bool s_local_frame_valid;
static volatile bool s_local_mjpeg_enabled;
static volatile int s_local_stream_socket = -1;
static httpd_handle_t s_local_httpd;

static const char LOCAL_VIEWER_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32 Camera</title><style>html,body{margin:0;background:#111;height:100%;}"
    "body{display:grid;place-items:center;}img{display:block;max-width:100%;max-height:100%;}"
    "</style></head><body><img src=\"/live/mjpeg\" alt=\"ESP32 camera\"></body></html>";


static uint8_t *allocate_local_frame_buffer(void)
{
    return heap_caps_malloc(MAX_JPEG_SIZE,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}


static esp_err_t ensure_local_frame_store(void)
{
    if (s_local_frame_lock == NULL) {
        s_local_frame_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_local_frame_lock != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to create local frame lock");
    }
    if (s_local_frame_buffer == NULL) {
        s_local_frame_buffer = allocate_local_frame_buffer();
        ESP_RETURN_ON_FALSE(s_local_frame_buffer != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to allocate local MJPEG frame buffer");
    }
    return ESP_OK;
}


static void publish_local_frame(const camera_fb_t *frame, uint32_t sequence)
{
    if (!s_local_mjpeg_enabled || s_local_frame_buffer == NULL ||
            s_local_frame_lock == NULL || frame->len > MAX_JPEG_SIZE) {
        return;
    }
    if (xSemaphoreTake(s_local_frame_lock, 0) != pdTRUE) {
        return;
    }
    memcpy(s_local_frame_buffer, frame->buf, frame->len);
    s_local_frame_length = frame->len;
    s_local_frame_sequence = sequence;
    s_local_frame_valid = true;
    xSemaphoreGive(s_local_frame_lock);
}


static bool copy_local_frame(uint8_t *destination, size_t *length,
                             uint32_t *sequence)
{
    if (destination == NULL || length == NULL || sequence == NULL ||
            s_local_frame_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_local_frame_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool ready = s_local_frame_valid && s_local_frame_length <= MAX_JPEG_SIZE;
    if (ready) {
        memcpy(destination, s_local_frame_buffer, s_local_frame_length);
        *length = s_local_frame_length;
        *sequence = s_local_frame_sequence;
    }
    xSemaphoreGive(s_local_frame_lock);
    return ready;
}


static esp_err_t local_viewer_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, LOCAL_VIEWER_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t local_mjpeg_handler(httpd_req_t *request);


static esp_err_t local_snapshot_handler(httpd_req_t *request)
{
    uint8_t *frame = allocate_local_frame_buffer();
    if (frame == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "frame buffer unavailable");
    }
    size_t length = 0;
    uint32_t sequence = 0;
    if (!copy_local_frame(frame, &length, &sequence)) {
        free(frame);
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_sendstr(request, "camera frame not ready");
    }
    (void)sequence;
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    esp_err_t result = httpd_resp_send(request, (const char *)frame, length);
    free(frame);
    return result;
}


esp_err_t camera_stream_set_local_mjpeg_enabled(bool enabled)
{
    if (enabled) {
        if (s_local_mjpeg_enabled && s_local_httpd != NULL) {
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(ensure_local_frame_store(), TAG,
                            "Local MJPEG frame store unavailable");
        if (xSemaphoreTake(s_local_frame_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_local_frame_valid = false;
            s_local_frame_length = 0;
            xSemaphoreGive(s_local_frame_lock);
        }

        if (s_local_httpd != NULL) {
            s_local_mjpeg_enabled = true;
            ESP_LOGI(TAG, "Local MJPEG server resumed");
            return ESP_OK;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = CONFIG_VIDEO_LOCAL_MJPEG_PORT;
        config.stack_size = 6144;
        config.max_open_sockets = 2;
        config.lru_purge_enable = true;
        config.send_wait_timeout = 1;

        s_local_mjpeg_enabled = true;
        esp_err_t result = httpd_start(&s_local_httpd, &config);
        if (result != ESP_OK) {
            s_local_mjpeg_enabled = false;
            s_local_httpd = NULL;
            ESP_LOGE(TAG, "Failed to start local MJPEG server: %s",
                     esp_err_to_name(result));
            return result;
        }

        static const httpd_uri_t viewer_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = local_viewer_handler,
            .user_ctx = NULL,
        };
        static const httpd_uri_t stream_uri = {
            .uri = "/live/mjpeg",
            .method = HTTP_GET,
            .handler = local_mjpeg_handler,
            .user_ctx = NULL,
        };
        static const httpd_uri_t snapshot_uri = {
            .uri = "/snapshot.jpg",
            .method = HTTP_GET,
            .handler = local_snapshot_handler,
            .user_ctx = NULL,
        };

        result = httpd_register_uri_handler(s_local_httpd, &viewer_uri);
        if (result == ESP_OK) {
            result = httpd_register_uri_handler(s_local_httpd, &stream_uri);
        }
        if (result == ESP_OK) {
            result = httpd_register_uri_handler(s_local_httpd, &snapshot_uri);
        }
        if (result != ESP_OK) {
            s_local_mjpeg_enabled = false;
            httpd_stop(s_local_httpd);
            s_local_httpd = NULL;
            ESP_LOGE(TAG, "Failed to register local MJPEG endpoint: %s",
                     esp_err_to_name(result));
            return result;
        }

        ESP_LOGI(TAG, "Local MJPEG ready: http://%s:%d/live/mjpeg",
                 CONFIG_SERVER_IP, CONFIG_VIDEO_LOCAL_MJPEG_PORT);
        ESP_LOGI(TAG, "Local camera viewer: http://%s:%d/",
                 CONFIG_SERVER_IP, CONFIG_VIDEO_LOCAL_MJPEG_PORT);
        return ESP_OK;
    }

    s_local_mjpeg_enabled = false;
    int stream_socket = s_local_stream_socket;
    if (stream_socket >= 0) {
        shutdown(stream_socket, SHUT_RDWR);
    }
    ESP_LOGI(TAG, "Local MJPEG server paused");
    return ESP_OK;
}


bool camera_stream_local_mjpeg_is_enabled(void)
{
    return s_local_mjpeg_enabled && s_local_httpd != NULL;
}


static esp_err_t local_mjpeg_handler(httpd_req_t *request)
{
    uint8_t *frame = allocate_local_frame_buffer();
    if (frame == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "frame buffer unavailable");
    }

    httpd_resp_set_type(request, LOCAL_MJPEG_TYPE);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    s_local_stream_socket = httpd_req_to_sockfd(request);
    uint32_t last_sequence = UINT32_MAX;
    esp_err_t result = ESP_OK;

    while (s_local_mjpeg_enabled) {
        size_t length = 0;
        uint32_t sequence = 0;
        if (!copy_local_frame(frame, &length, &sequence) || sequence == last_sequence) {
            vTaskDelay(pdMS_TO_TICKS(LOCAL_MJPEG_FRAME_WAIT_MS));
            continue;
        }

        char header[128];
        int header_length = snprintf(
            header, sizeof(header),
            LOCAL_MJPEG_PREFIX "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            (unsigned)length);
        if (header_length <= 0 || header_length >= (int)sizeof(header) ||
                httpd_resp_send_chunk(request, header, header_length) != ESP_OK ||
                httpd_resp_send_chunk(request, (const char *)frame, length) != ESP_OK ||
                httpd_resp_send_chunk(request, "\r\n", 2) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        last_sequence = sequence;
    }

    s_local_stream_socket = -1;
    free(frame);
    if (!s_local_mjpeg_enabled) {
        result = ESP_FAIL;
    }
    return result;
}


static void put_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}


static void put_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}


static bool jpeg_is_complete(const camera_fb_t *frame)
{
    return frame != NULL && frame->format == PIXFORMAT_JPEG && frame->len >= 4 &&
           frame->buf[0] == 0xff && frame->buf[1] == 0xd8 &&
           frame->buf[frame->len - 2] == 0xff && frame->buf[frame->len - 1] == 0xd9;
}


static esp_err_t camera_init(void)
{
    const bool has_psram = esp_psram_is_initialized();
    const size_t frame_buffer_count = has_psram ? 2U : 1U;
    const camera_grab_mode_t grab_mode =
        has_psram ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 24000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CAMERA_FRAME_SIZE,
        .jpeg_quality = CAMERA_JPEG_QUALITY,
        /* Speed-first mode: continuous JPEG capture and discard stale frames. */
        .fb_count = frame_buffer_count,
        .grab_mode = grab_mode,
        .fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
    };

    ESP_LOGI(TAG,
             "Initializing OV5640 JPEG %s (quality=%d, PSRAM=%s, fb=%u, grab=%s)",
             CAMERA_FRAME_SIZE_NAME, CAMERA_JPEG_QUALITY,
             has_psram ? "enabled" : "not detected",
             (unsigned)frame_buffer_count,
             grab_mode == CAMERA_GRAB_LATEST ? "LATEST" : "WHEN_EMPTY");
    esp_err_t error = esp_camera_init(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OV5640 initialization failed: %s", esp_err_to_name(error));
        return error;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        ESP_LOGI(TAG, "Camera sensor PID=0x%04X, XCLK=%u Hz", sensor->id.PID,
                 (unsigned)sensor->xclk_freq_hz);
    }
    s_camera_initialized = true;
    return ESP_OK;
}


static int create_udp_socket(void)
{
    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_VIDEO_UDP_SERVER_PORT),
    };
    if (inet_pton(AF_INET, CONFIG_VIDEO_UDP_SERVER_IP, &destination.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid UDP server IPv4 address: %s", CONFIG_VIDEO_UDP_SERVER_IP);
        return -1;
    }

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "UDP socket creation failed: errno=%d", errno);
        return -1;
    }

    if (s_uplink_ifname[0] != '\0') {
        struct ifreq interface = {0};
        snprintf(interface.ifr_name, sizeof(interface.ifr_name), "%s",
                 s_uplink_ifname);
        if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE,
                       &interface, sizeof(interface)) != 0) {
            ESP_LOGE(TAG, "Failed to bind video UDP socket to %s: errno=%d",
                     s_uplink_ifname, errno);
            close(socket_fd);
            return -1;
        }
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(socket_fd, (struct sockaddr *)&destination, sizeof(destination)) != 0) {
        ESP_LOGE(TAG, "UDP connect failed: errno=%d", errno);
        close(socket_fd);
        return -1;
    }
    ESP_LOGI(TAG, "UDP uplink ready -> %s:%d", CONFIG_VIDEO_UDP_SERVER_IP,
             CONFIG_VIDEO_UDP_SERVER_PORT);
    return socket_fd;
}


static void close_udp_flows(int *socket_fds)
{
    for (int index = 0; index < CONFIG_VIDEO_UDP_FLOW_COUNT; ++index) {
        if (socket_fds[index] >= 0) {
            close(socket_fds[index]);
            socket_fds[index] = -1;
        }
    }
}


static bool create_udp_flows(int *socket_fds)
{
    for (int index = 0; index < CONFIG_VIDEO_UDP_FLOW_COUNT; ++index) {
        socket_fds[index] = create_udp_socket();
        if (socket_fds[index] < 0) {
            close_udp_flows(socket_fds);
            return false;
        }
    }
    ESP_LOGI(TAG, "%d parallel UDP video flows ready", CONFIG_VIDEO_UDP_FLOW_COUNT);
    return true;
}


static bool send_jpeg_frame(int socket_fd, uint8_t *packet, const camera_fb_t *frame,
                            uint32_t device_id, uint32_t frame_sequence)
{
    const uint16_t chunk_count = (uint16_t)((frame->len + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE);
    const uint32_t frame_crc32 = esp_rom_crc32_le(0, frame->buf, frame->len);
    const uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        if ((xEventGroupGetBits(s_network_events) & NETWORK_READY_BIT) == 0) {
            return false;
        }
        const size_t offset = (size_t)chunk_index * UDP_PAYLOAD_SIZE;
        const uint16_t payload_length = (uint16_t)((frame->len - offset) > UDP_PAYLOAD_SIZE
                                                ? UDP_PAYLOAD_SIZE : (frame->len - offset));

        memcpy(packet, "ESJP", 4);
        packet[4] = ESJP_VERSION;
        packet[5] = 0;
        put_u16_be(packet + 6, ESJP_HEADER_SIZE);
        put_u32_be(packet + 8, device_id);
        put_u32_be(packet + 12, frame_sequence);
        put_u32_be(packet + 16, timestamp_ms);
        put_u32_be(packet + 20, (uint32_t)frame->len);
        put_u32_be(packet + 24, frame_crc32);
        put_u16_be(packet + 28, chunk_index);
        put_u16_be(packet + 30, chunk_count);
        put_u16_be(packet + 32, payload_length);
        put_u16_be(packet + 34, (uint16_t)frame->width);
        put_u16_be(packet + 36, (uint16_t)frame->height);
        memcpy(packet + ESJP_HEADER_SIZE, frame->buf + offset, payload_length);

        const size_t packet_length = ESJP_HEADER_SIZE + payload_length;
        ssize_t sent = send(socket_fd, packet, packet_length, 0);
        if (sent != (ssize_t)packet_length) {
            ESP_LOGW(TAG, "UDP send failed at chunk %u/%u: sent=%d errno=%d",
                     (unsigned)chunk_index + 1, (unsigned)chunk_count, (int)sent, errno);
            return false;
        }

        const bool burst_complete =
            ((chunk_index + 1U) % CONFIG_VIDEO_UDP_BURST_PACKETS) == 0U;
        const bool more_chunks = (chunk_index + 1U) < chunk_count;
        if (burst_complete && more_chunks && CONFIG_VIDEO_UDP_BURST_YIELD_MS > 0) {
            /* Let USB Host/RNDIS drain its short TX queue without pacing every packet. */
            vTaskDelay(pdMS_TO_TICKS(CONFIG_VIDEO_UDP_BURST_YIELD_MS));
        }
    }
    return true;
}


static int upload_fps_for_link_age(int64_t link_ready_us)
{
#ifdef CONFIG_VIDEO_UDP_EXTENDED_RAMP
    const int64_t link_age_us = esp_timer_get_time() - link_ready_us;
    int upload_fps;
    if (link_age_us < 15LL * 1000000LL) {
        upload_fps = 5;
    } else if (link_age_us < 35LL * 1000000LL) {
        upload_fps = 8;
    } else if (link_age_us < 65LL * 1000000LL) {
        upload_fps = 10;
    } else if (link_age_us < 95LL * 1000000LL) {
        upload_fps = 12;
    } else if (link_age_us < 125LL * 1000000LL) {
        upload_fps = 15;
    } else if (link_age_us < 155LL * 1000000LL) {
        upload_fps = 20;
    } else {
        upload_fps = CONFIG_VIDEO_UDP_FPS;
    }
#else
    if (CONFIG_VIDEO_UDP_WARMUP_STEP_SECONDS == 0) {
        return CONFIG_VIDEO_UDP_FPS;
    }

    const int64_t step_us =
        (int64_t)CONFIG_VIDEO_UDP_WARMUP_STEP_SECONDS * 1000000LL;
    const int64_t link_age_us = esp_timer_get_time() - link_ready_us;
    int upload_fps;
    if (link_age_us < step_us) {
        upload_fps = 5;
    } else if (link_age_us < step_us * 2) {
        upload_fps = 10;
    } else if (link_age_us < step_us * 3) {
        upload_fps = 20;
    } else {
        upload_fps = CONFIG_VIDEO_UDP_FPS;
    }
#endif
    return upload_fps < CONFIG_VIDEO_UDP_FPS ? upload_fps : CONFIG_VIDEO_UDP_FPS;
}


static void camera_udp_task(void *argument)
{
    (void)argument;
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    const uint32_t device_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                               ((uint32_t)mac[4] << 8) | mac[5];
    uint8_t *packet = malloc(ESJP_HEADER_SIZE + UDP_PAYLOAD_SIZE);
    if (packet == NULL) {
        ESP_LOGE(TAG, "Failed to allocate UDP packet buffer");
        vTaskDelete(NULL);
        return;
    }

    int socket_fds[CONFIG_VIDEO_UDP_FLOW_COUNT];
    for (int index = 0; index < CONFIG_VIDEO_UDP_FLOW_COUNT; ++index) {
        socket_fds[index] = -1;
    }
    int consecutive_failures = 0;
    uint32_t sequence = 0;
    uint32_t completed_frames = 0;
    uint32_t dropped_frames = 0;
    uint32_t report_frames = 0;
    uint64_t completed_bytes = 0;
    size_t report_max_jpeg_bytes = 0;
    int64_t report_start_us = esp_timer_get_time();
    int64_t link_ready_us = 0;
    int64_t next_socket_retry_us = 0;
    int active_upload_fps = 0;

    ESP_LOGI(TAG, "ESJP device ID=%08lX; waiting for L610 RNDIS IPv4",
             (unsigned long)device_id);
    while (true) {
        const bool network_ready =
            (xEventGroupGetBits(s_network_events) & NETWORK_READY_BIT) != 0;
        if (!network_ready && !s_local_mjpeg_enabled) {
            xEventGroupWaitBits(s_network_events, NETWORK_READY_BIT, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(100));
            continue;
        }
        if (!network_ready && socket_fds[0] >= 0) {
            close_udp_flows(socket_fds);
            active_upload_fps = 0;
            next_socket_retry_us = 0;
        }

        bool cloud_ready = network_ready && socket_fds[0] >= 0;
        const int64_t now_us = esp_timer_get_time();
        if (network_ready && socket_fds[0] < 0 && now_us >= next_socket_retry_us) {
            if (create_udp_flows(socket_fds)) {
                link_ready_us = now_us;
                active_upload_fps = 0;
                report_start_us = link_ready_us;
                report_frames = 0;
                completed_bytes = 0;
                report_max_jpeg_bytes = 0;
                consecutive_failures = 0;
                cloud_ready = true;
            } else {
                next_socket_retry_us = now_us + 1000000LL;
            }
        }

        const int selected_upload_fps = cloud_ready
            ? upload_fps_for_link_age(link_ready_us) : CONFIG_VIDEO_UDP_FPS;
        if (cloud_ready && selected_upload_fps != active_upload_fps) {
            active_upload_fps = selected_upload_fps;
#ifdef CONFIG_VIDEO_UDP_EXTENDED_RAMP
            ESP_LOGI(TAG, "RNDIS video extended ramp transition -> %d FPS (link age %.1f s)",
                     active_upload_fps,
                     (esp_timer_get_time() - link_ready_us) / 1000000.0);
#else
            ESP_LOGI(TAG, "RNDIS video startup ramp -> %d FPS", active_upload_fps);
#endif
        }
        const int64_t frame_interval_us = 1000000LL / selected_upload_fps;
        const int64_t frame_start_us = esp_timer_get_time();
        camera_fb_t *frame = esp_camera_fb_get();
        if (!jpeg_is_complete(frame) || frame->len > MAX_JPEG_SIZE) {
            if (frame != NULL) {
                ESP_LOGW(TAG, "Dropping invalid JPEG: %ux%u, %u bytes", frame->width,
                         frame->height, (unsigned)frame->len);
                esp_camera_fb_return(frame);
            } else {
                ESP_LOGW(TAG, "Camera capture returned no frame");
            }
            dropped_frames++;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const size_t frame_length = frame->len;
        const uint16_t frame_width = frame->width;
        const uint16_t frame_height = frame->height;
        const uint32_t frame_sequence = sequence++;
        publish_local_frame(frame, frame_sequence);
        bool sent = true;
        if (cloud_ready) {
            const int flow_index = frame_sequence % CONFIG_VIDEO_UDP_FLOW_COUNT;
            sent = send_jpeg_frame(socket_fds[flow_index], packet, frame,
                                   device_id, frame_sequence);
        }
        esp_camera_fb_return(frame);

        if (cloud_ready && !sent) {
            dropped_frames++;
            consecutive_failures++;
            close_udp_flows(socket_fds);
            next_socket_retry_us = esp_timer_get_time() + 1000000LL;
            if (consecutive_failures >= SEND_FAILURE_LIMIT) {
                ESP_LOGW(TAG, "%d consecutive UDP frame failures; waiting before reconnect",
                         consecutive_failures);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }

        if (!cloud_ready) {
            const int64_t elapsed_us = esp_timer_get_time() - frame_start_us;
            if (elapsed_us < frame_interval_us) {
                vTaskDelay(pdMS_TO_TICKS((frame_interval_us - elapsed_us) / 1000));
            }
            continue;
        }

        consecutive_failures = 0;
        completed_frames++;
        report_frames++;
        completed_bytes += frame_length;
        if (frame_length > report_max_jpeg_bytes) {
            report_max_jpeg_bytes = frame_length;
        }
        if (completed_frames <= 3) {
            ESP_LOGI(TAG, "Sent JPEG #%lu: %ux%u, %u bytes",
                     (unsigned long)completed_frames, frame_width, frame_height,
                     (unsigned)frame_length);
        }
        if (report_frames == 50) {
            const int64_t now_us = esp_timer_get_time();
            const double seconds = (now_us - report_start_us) / 1000000.0;
            const double average_jpeg_bytes = (double)completed_bytes / report_frames;
            ESP_LOGI(TAG,
                     "UDP video: %.2f FPS, %.1f KiB/s, JPEG avg=%.0f B max=%u B, dropped=%lu",
                     report_frames / seconds, (completed_bytes / 1024.0) / seconds,
                     average_jpeg_bytes, (unsigned)report_max_jpeg_bytes,
                     (unsigned long)dropped_frames);
            report_start_us = now_us;
            report_frames = 0;
            completed_bytes = 0;
            report_max_jpeg_bytes = 0;
        }

        const int64_t elapsed_us = esp_timer_get_time() - frame_start_us;
        if (elapsed_us < frame_interval_us) {
            vTaskDelay(pdMS_TO_TICKS((frame_interval_us - elapsed_us) / 1000));
        }
    }
}


void camera_stream_set_network_ready(bool ready)
{
    if (s_network_events == NULL) {
        return;
    }
    if (ready) {
        xEventGroupSetBits(s_network_events, NETWORK_READY_BIT);
    } else {
        xEventGroupClearBits(s_network_events, NETWORK_READY_BIT);
    }
}

void camera_stream_set_uplink_interface(const char *ifname)
{
    snprintf(s_uplink_ifname, sizeof(s_uplink_ifname), "%s",
             ifname == NULL ? "" : ifname);
    ESP_LOGI(TAG, "Video UDP uplink interface=%s",
             s_uplink_ifname[0] == '\0' ? "default" : s_uplink_ifname);
}


esp_err_t camera_stream_start(void)
{
    if (s_task_started) {
        return ESP_OK;
    }
    if (s_network_events == NULL) {
        s_network_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_network_events != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to create camera network event group");
    }
    if (!s_camera_initialized) {
        ESP_RETURN_ON_ERROR(camera_init(), TAG, "Camera unavailable");
    }
    BaseType_t created = xTaskCreate(camera_udp_task, "camera_udp", 6144, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "Failed to create camera UDP task");
    s_task_started = true;
    ESP_LOGI(TAG,
             "Camera UDP uploader configured for %s:%d at %d FPS, ESJP v%d payload=%d, "
             "flows=%d, burst=%d packets/%d ms",
             CONFIG_VIDEO_UDP_SERVER_IP, CONFIG_VIDEO_UDP_SERVER_PORT,
             CONFIG_VIDEO_UDP_FPS, CONFIG_VIDEO_UDP_PROTOCOL_VERSION,
             CONFIG_VIDEO_UDP_PAYLOAD_SIZE, CONFIG_VIDEO_UDP_FLOW_COUNT,
             CONFIG_VIDEO_UDP_BURST_PACKETS,
             CONFIG_VIDEO_UDP_BURST_YIELD_MS);
#ifdef CONFIG_VIDEO_UDP_EXTENDED_RAMP
    ESP_LOGI(TAG,
             "RNDIS extended startup ramp: 5 FPS/15 s -> 8 FPS/20 s -> "
             "10 FPS/30 s -> 12 FPS/30 s -> 15 FPS/30 s -> 20 FPS/30 s -> "
             "target %d FPS",
             CONFIG_VIDEO_UDP_FPS);
#else
    if (CONFIG_VIDEO_UDP_WARMUP_STEP_SECONDS > 0) {
        ESP_LOGI(TAG, "RNDIS startup ramp: 5 -> 10 -> 20 -> %d FPS, %d seconds per step",
                 CONFIG_VIDEO_UDP_FPS, CONFIG_VIDEO_UDP_WARMUP_STEP_SECONDS);
    }
#endif
    return ESP_OK;
}
