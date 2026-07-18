#include "telemetry_uart.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TELEMETRY_MAGIC_0 0xA5
#define TELEMETRY_MAGIC_1 0x5A
#define TELEMETRY_VERSION 1
#define TELEMETRY_TYPE_SENSOR_GPS 1
#define TELEMETRY_TYPE_PS2_CONTROL 2
#define TELEMETRY_FIXED_HEADER 10U
#define TELEMETRY_CRC_SIZE 2U
#define TELEMETRY_TAIL_SIZE 2U
#define TELEMETRY_MAX_JSON 1024U
#define TELEMETRY_MAX_FRAME (TELEMETRY_FIXED_HEADER + TELEMETRY_MAX_JSON + \
                             TELEMETRY_CRC_SIZE + TELEMETRY_TAIL_SIZE)
#define CONTROL_MAX_JSON 512U
#define CONTROL_MAX_REQUEST_ID 64U
#define CONTROL_MAX_BUTTON 9U
#define CONTROL_MAX_STATE 4U
#define PS2_UART_JSON_MAX 64U

static const char *TAG = "TELEMETRY_UART";
static volatile bool s_network_ready;
static char s_uplink_ifname[IFNAMSIZ];
static volatile uint32_t s_uplink_generation;
static bool s_started;
static char s_boot_id[9];
static portMUX_TYPE s_ap_stream_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ap_stream_supported;
static telemetry_ap_stream_state_t s_ap_stream_state = TELEMETRY_AP_STREAM_UNSUPPORTED;
static esp_err_t s_ap_stream_error = ESP_OK;
static char s_ap_stream_request_id[CONTROL_MAX_REQUEST_ID + 1];
static bool s_ap_stream_last_command_valid;
static bool s_ap_stream_last_command_enabled;
static telemetry_ap_stream_control_cb_t s_ap_stream_control_cb;
static void *s_ap_stream_control_ctx;
static portMUX_TYPE s_modem_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_modem_supported;
static telemetry_modem_state_t s_modem_state = {
    .state = TELEMETRY_MODEM_UNSUPPORTED,
    .registration = -1,
    .rssi = -1,
};
static telemetry_modem_control_cb_t s_modem_control_cb;
static void *s_modem_control_ctx;
static portMUX_TYPE s_video_fps_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_video_fps_supported;
static int s_video_fps = 8;
static char s_video_resolution[TELEMETRY_VIDEO_RESOLUTION_MAX + 1] = "640x480";
static char s_video_fps_request_id[CONTROL_MAX_REQUEST_ID + 1];
static esp_err_t s_video_fps_error = ESP_OK;
static telemetry_video_fps_control_cb_t s_video_fps_control_cb;
static void *s_video_fps_control_ctx;
static portMUX_TYPE s_wifi_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_wifi_supported;
static telemetry_wifi_state_t s_wifi_state = {
    .state = TELEMETRY_WIFI_IDLE,
    .action = TELEMETRY_WIFI_ACTION_QUERY,
    .active_uplink = "none",
};
static telemetry_wifi_control_cb_t s_wifi_control_cb;
static void *s_wifi_control_ctx;

typedef struct {
    bool supported;
    telemetry_ap_stream_state_t state;
    esp_err_t error;
    char request_id[CONTROL_MAX_REQUEST_ID + 1];
    bool last_command_valid;
    bool last_command_enabled;
    telemetry_ap_stream_control_cb_t control_cb;
    void *control_ctx;
} ap_stream_snapshot_t;

typedef struct {
    bool supported;
    telemetry_modem_state_t value;
    telemetry_modem_control_cb_t control_cb;
    void *control_ctx;
} modem_snapshot_t;

typedef struct {
    bool supported;
    int fps;
    char resolution[TELEMETRY_VIDEO_RESOLUTION_MAX + 1];
    char request_id[CONTROL_MAX_REQUEST_ID + 1];
    esp_err_t error;
    telemetry_video_fps_control_cb_t control_cb;
    void *control_ctx;
} video_fps_snapshot_t;

typedef struct {
    bool supported;
    telemetry_wifi_state_t value;
    telemetry_wifi_control_cb_t control_cb;
    void *control_ctx;
} wifi_snapshot_t;

typedef enum {
    CONTROL_COMMAND_AP_STREAM,
    CONTROL_COMMAND_PS2_BUTTON,
    CONTROL_COMMAND_MODEM_4G,
    CONTROL_COMMAND_VIDEO_FPS,
    CONTROL_COMMAND_WIFI_STA,
} control_command_kind_t;

typedef struct {
    control_command_kind_t kind;
    char request_id[CONTROL_MAX_REQUEST_ID + 1];
    union {
        bool ap_stream_enabled;
        struct {
            char button[CONTROL_MAX_BUTTON + 1];
            char state[CONTROL_MAX_STATE + 1];
        } ps2;
        telemetry_modem_command_t modem;
        struct {
            int fps;
            bool resolution_set;
            char resolution[TELEMETRY_VIDEO_RESOLUTION_MAX + 1];
        } video_fps;
        telemetry_wifi_command_t wifi;
    } data;
} control_command_t;

static const uint16_t s_supported_lte_bands[] = {
    1, 3, 5, 7, 8, 20, 34, 39, 40, 41,
};

static const char *ap_stream_state_name(telemetry_ap_stream_state_t state)
{
    switch (state) {
    case TELEMETRY_AP_STREAM_STARTING:
        return "starting";
    case TELEMETRY_AP_STREAM_ENABLED:
        return "enabled";
    case TELEMETRY_AP_STREAM_STOPPING:
        return "stopping";
    case TELEMETRY_AP_STREAM_DISABLED:
        return "disabled";
    case TELEMETRY_AP_STREAM_ERROR:
        return "error";
    case TELEMETRY_AP_STREAM_UNSUPPORTED:
    default:
        /* supported=false carries capability; disabled keeps the shared state contract. */
        return "disabled";
    }
}

static void get_ap_stream_snapshot(ap_stream_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_ap_stream_mux);
    snapshot->supported = s_ap_stream_supported;
    snapshot->state = s_ap_stream_state;
    snapshot->error = s_ap_stream_error;
    memcpy(snapshot->request_id, s_ap_stream_request_id,
           sizeof(snapshot->request_id));
    snapshot->last_command_valid = s_ap_stream_last_command_valid;
    snapshot->last_command_enabled = s_ap_stream_last_command_enabled;
    snapshot->control_cb = s_ap_stream_control_cb;
    snapshot->control_ctx = s_ap_stream_control_ctx;
    portEXIT_CRITICAL(&s_ap_stream_mux);
}

static void ap_stream_set_command_state(const char *request_id, bool enabled,
                                        telemetry_ap_stream_state_t state,
                                        esp_err_t error)
{
    portENTER_CRITICAL(&s_ap_stream_mux);
    snprintf(s_ap_stream_request_id, sizeof(s_ap_stream_request_id), "%s", request_id);
    s_ap_stream_last_command_valid = true;
    s_ap_stream_last_command_enabled = enabled;
    s_ap_stream_state = state;
    s_ap_stream_error = error;
    portEXIT_CRITICAL(&s_ap_stream_mux);
}

static const char *modem_state_name(telemetry_modem_state_code_t state)
{
    switch (state) {
    case TELEMETRY_MODEM_IDLE:
        return "idle";
    case TELEMETRY_MODEM_QUERYING:
        return "querying";
    case TELEMETRY_MODEM_APPLYING:
        return "applying";
    case TELEMETRY_MODEM_SUCCESS:
        return "success";
    case TELEMETRY_MODEM_ERROR:
        return "error";
    case TELEMETRY_MODEM_UNSUPPORTED:
    default:
        return "unsupported";
    }
}

static const char *modem_action_name(telemetry_modem_action_t action)
{
    switch (action) {
    case TELEMETRY_MODEM_ACTION_PING:
        return "ping";
    case TELEMETRY_MODEM_ACTION_RESELECT:
        return "reselect";
    case TELEMETRY_MODEM_ACTION_SET_BANDS:
        return "set_bands";
    case TELEMETRY_MODEM_ACTION_SET_CELL_LOCK:
        return "set_cell_lock";
    case TELEMETRY_MODEM_ACTION_CLEAR_CELL_LOCK:
        return "clear_cell_lock";
    case TELEMETRY_MODEM_ACTION_QUERY:
    default:
        return "query";
    }
}

static const char *wifi_state_name(telemetry_wifi_state_code_t state)
{
    switch (state) {
    case TELEMETRY_WIFI_APPLYING:
        return "applying";
    case TELEMETRY_WIFI_SUCCESS:
        return "success";
    case TELEMETRY_WIFI_ERROR:
        return "error";
    case TELEMETRY_WIFI_IDLE:
    default:
        return "idle";
    }
}

static const char *wifi_action_name(telemetry_wifi_action_t action)
{
    switch (action) {
    case TELEMETRY_WIFI_ACTION_SET_ENABLED:
        return "set_enabled";
    case TELEMETRY_WIFI_ACTION_SCAN:
        return "scan";
    case TELEMETRY_WIFI_ACTION_CONNECT:
        return "connect";
    case TELEMETRY_WIFI_ACTION_SELECT_UPLINK:
        return "select_uplink";
    case TELEMETRY_WIFI_ACTION_QUERY:
    default:
        return "query";
    }
}

static void get_wifi_snapshot(wifi_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_wifi_mux);
    snapshot->supported = s_wifi_supported;
    snapshot->value = s_wifi_state;
    snapshot->control_cb = s_wifi_control_cb;
    snapshot->control_ctx = s_wifi_control_ctx;
    portEXIT_CRITICAL(&s_wifi_mux);
}

static void wifi_set_command_state(const telemetry_wifi_command_t *command,
                                   telemetry_wifi_state_code_t state,
                                   const char *error)
{
    portENTER_CRITICAL(&s_wifi_mux);
    s_wifi_state.state = state;
    s_wifi_state.action = command->action;
    snprintf(s_wifi_state.request_id, sizeof(s_wifi_state.request_id), "%s",
             command->request_id);
    snprintf(s_wifi_state.error, sizeof(s_wifi_state.error), "%s",
             error == NULL ? "" : error);
    portEXIT_CRITICAL(&s_wifi_mux);
}

static void get_video_fps_snapshot(video_fps_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_video_fps_mux);
    snapshot->supported = s_video_fps_supported;
    snapshot->fps = s_video_fps;
    memcpy(snapshot->resolution, s_video_resolution,
           sizeof(snapshot->resolution));
    memcpy(snapshot->request_id, s_video_fps_request_id,
           sizeof(snapshot->request_id));
    snapshot->error = s_video_fps_error;
    snapshot->control_cb = s_video_fps_control_cb;
    snapshot->control_ctx = s_video_fps_control_ctx;
    portEXIT_CRITICAL(&s_video_fps_mux);
}

static bool video_resolution_dimensions(const char *resolution,
                                        int *width, int *height)
{
    if (resolution == NULL) {
        return false;
    }
    int parsed_width = 0;
    int parsed_height = 0;
    if (strcmp(resolution, "640x480") == 0) {
        parsed_width = 640;
        parsed_height = 480;
    } else if (strcmp(resolution, "1280x720") == 0) {
        parsed_width = 1280;
        parsed_height = 720;
    } else if (strcmp(resolution, "1920x1080") == 0) {
        parsed_width = 1920;
        parsed_height = 1080;
    } else {
        return false;
    }
    if (width != NULL) {
        *width = parsed_width;
    }
    if (height != NULL) {
        *height = parsed_height;
    }
    return true;
}

static void set_video_fps_state(const char *request_id, int fps,
                                const char *resolution, esp_err_t error)
{
    portENTER_CRITICAL(&s_video_fps_mux);
    snprintf(s_video_fps_request_id, sizeof(s_video_fps_request_id), "%s",
             request_id);
    if (error == ESP_OK) {
        s_video_fps = fps;
        if (resolution != NULL) {
            snprintf(s_video_resolution, sizeof(s_video_resolution), "%s",
                     resolution);
        }
    }
    s_video_fps_error = error;
    portEXIT_CRITICAL(&s_video_fps_mux);
}

static void get_modem_snapshot(modem_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_modem_mux);
    snapshot->supported = s_modem_supported;
    snapshot->value = s_modem_state;
    snapshot->control_cb = s_modem_control_cb;
    snapshot->control_ctx = s_modem_control_ctx;
    portEXIT_CRITICAL(&s_modem_mux);
}

static void modem_set_command_state(const telemetry_modem_command_t *command,
                                    telemetry_modem_state_code_t state,
                                    const char *error)
{
    portENTER_CRITICAL(&s_modem_mux);
    s_modem_state.state = state;
    s_modem_state.action = command->action;
    snprintf(s_modem_state.request_id, sizeof(s_modem_state.request_id), "%s",
             command->request_id);
    snprintf(s_modem_state.error, sizeof(s_modem_state.error), "%s",
             error == NULL ? "" : error);
    portEXIT_CRITICAL(&s_modem_mux);
}

static uart_port_t telemetry_uart_port(void)
{
#if CONFIG_TELEMETRY_UART_SHARED_CONSOLE_TEST
    return (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
#else
    return (uart_port_t)CONFIG_TELEMETRY_UART_NUM;
#endif
}

typedef struct {
    uint8_t data[TELEMETRY_MAX_FRAME];
    size_t used;
    size_t expected;
    int64_t started_us;
    uint32_t valid_frames;
    uint32_t dropped_frames;
    uint32_t crc_errors;
    uint32_t tail_errors;
    uint32_t timeout_errors;
} frame_parser_t;

static uint16_t read_u16_be(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

#if !CONFIG_TELEMETRY_UART_SHARED_CONSOLE_TEST
static void write_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}
#endif

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void parser_reset(frame_parser_t *parser)
{
    parser->used = 0;
    parser->expected = 0;
    parser->started_us = 0;
}

static void parser_drop(frame_parser_t *parser, const char *reason)
{
    parser->dropped_frames++;
    if (parser->dropped_frames <= 5 || parser->dropped_frames % 50 == 0) {
        ESP_LOGW(TAG, "USART frame dropped (%s), total=%" PRIu32, reason, parser->dropped_frames);
    }
    parser_reset(parser);
}

static int open_udp_socket(void)
{
    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_TELEMETRY_UDP_SERVER_PORT),
    };
    if (inet_pton(AF_INET, CONFIG_TELEMETRY_UDP_SERVER_IP, &destination.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid telemetry server IPv4: %s", CONFIG_TELEMETRY_UDP_SERVER_IP);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "UDP socket failed: errno=%d", errno);
        return -1;
    }
    if (s_uplink_ifname[0] != '\0') {
        struct ifreq interface = {0};
        snprintf(interface.ifr_name, sizeof(interface.ifr_name), "%s",
                 s_uplink_ifname);
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       &interface, sizeof(interface)) != 0) {
            ESP_LOGE(TAG, "Failed to bind telemetry UDP socket to %s: errno=%d",
                     s_uplink_ifname, errno);
            close(fd);
            return -1;
        }
    }
    if (connect(fd, (struct sockaddr *)&destination, sizeof(destination)) != 0) {
        ESP_LOGE(TAG, "UDP connect failed: errno=%d", errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "Unified telemetry UDP -> %s:%d", CONFIG_TELEMETRY_UDP_SERVER_IP,
             CONFIG_TELEMETRY_UDP_SERVER_PORT);
    return fd;
}

static bool valid_request_id(const char *request_id)
{
    size_t length = request_id == NULL ? 0 : strlen(request_id);
    if (length == 0 || length > CONTROL_MAX_REQUEST_ID) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char ch = request_id[i];
        const bool allowed = (ch >= 'a' && ch <= 'z') ||
                             (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') ||
                             ch == '-' || ch == '_' || ch == '.' || ch == ':';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool valid_ps2_button(const char *button)
{
    static const char *const buttons[] = {
        "PAD_UP", "PAD_RIGHT", "PAD_DOWN", "PAD_LEFT",
        "L2", "R2", "L1", "R1",
        "TRIANGLE", "CIRCLE", "CROSS", "SQUARE", "SELECT", "START",
    };
    if (button == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        if (strcmp(button, buttons[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool valid_ps2_state(const char *state)
{
    return state != NULL && (strcmp(state, "down") == 0 || strcmp(state, "up") == 0);
}

static bool supported_lte_band(int band)
{
    for (size_t i = 0;
            i < sizeof(s_supported_lte_bands) / sizeof(s_supported_lte_bands[0]); ++i) {
        if (band == s_supported_lte_bands[i]) {
            return true;
        }
    }
    return false;
}

static bool parse_modem_command(const cJSON *params,
                                telemetry_modem_command_t *output)
{
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(params, "action");
    if (!cJSON_IsString(action)) {
        return false;
    }
    if (strcmp(action->valuestring, "query") == 0) {
        output->action = TELEMETRY_MODEM_ACTION_QUERY;
        return true;
    }
    if (strcmp(action->valuestring, "ping") == 0) {
        output->action = TELEMETRY_MODEM_ACTION_PING;
        return true;
    }
    if (strcmp(action->valuestring, "reselect") == 0) {
        output->action = TELEMETRY_MODEM_ACTION_RESELECT;
        return true;
    }
    if (strcmp(action->valuestring, "clear_cell_lock") == 0) {
        output->action = TELEMETRY_MODEM_ACTION_CLEAR_CELL_LOCK;
        return true;
    }
    if (strcmp(action->valuestring, "set_bands") == 0) {
        const cJSON *bands = cJSON_GetObjectItemCaseSensitive(params, "bands");
        const int count = cJSON_IsArray(bands) ? cJSON_GetArraySize(bands) : 0;
        if (count <= 0 || count > TELEMETRY_MODEM_MAX_BANDS) {
            return false;
        }
        output->action = TELEMETRY_MODEM_ACTION_SET_BANDS;
        output->band_count = (size_t)count;
        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(bands, i);
            if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
                    !supported_lte_band(item->valueint)) {
                return false;
            }
            for (int previous = 0; previous < i; ++previous) {
                if (output->bands[previous] == (uint16_t)item->valueint) {
                    return false;
                }
            }
            output->bands[i] = (uint16_t)item->valueint;
        }
        return true;
    }
    if (strcmp(action->valuestring, "set_cell_lock") == 0) {
        const cJSON *earfcn = cJSON_GetObjectItemCaseSensitive(params, "earfcn");
        const cJSON *pci = cJSON_GetObjectItemCaseSensitive(params, "pci");
        if (!cJSON_IsNumber(earfcn) || earfcn->valuedouble != earfcn->valueint ||
                earfcn->valuedouble < 0 || earfcn->valuedouble > UINT32_MAX) {
            return false;
        }
        output->action = TELEMETRY_MODEM_ACTION_SET_CELL_LOCK;
        output->earfcn = (uint32_t)earfcn->valuedouble;
        output->pci = -1;
        output->lock_pci = false;
        if (pci != NULL && !cJSON_IsNull(pci)) {
            if (!cJSON_IsNumber(pci) || pci->valuedouble != pci->valueint ||
                    pci->valueint < 0 || pci->valueint > 503) {
                return false;
            }
            output->pci = pci->valueint;
            output->lock_pci = true;
        }
        return true;
    }
    return false;
}

static bool valid_wifi_security(const char *security)
{
    static const char *const values[] = {
        "open", "wpa", "wpa2", "wpa/wpa2", "wpa3", "wpa2/wpa3",
    };
    if (security == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (strcmp(security, values[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_wifi_command(const cJSON *params,
                               telemetry_wifi_command_t *output)
{
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(params, "action");
    if (!cJSON_IsString(action)) {
        return false;
    }
    if (strcmp(action->valuestring, "query") == 0) {
        output->action = TELEMETRY_WIFI_ACTION_QUERY;
        return true;
    }
    if (strcmp(action->valuestring, "set_enabled") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled");
        if (!cJSON_IsBool(enabled)) {
            return false;
        }
        output->action = TELEMETRY_WIFI_ACTION_SET_ENABLED;
        output->enabled = cJSON_IsTrue(enabled);
        return true;
    }
    if (strcmp(action->valuestring, "scan") == 0) {
        output->action = TELEMETRY_WIFI_ACTION_SCAN;
        return true;
    }
    if (strcmp(action->valuestring, "connect") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "password");
        const cJSON *security = cJSON_GetObjectItemCaseSensitive(params, "security");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
                !cJSON_IsString(security)) {
            return false;
        }
        const size_t ssid_length = strlen(ssid->valuestring);
        const size_t password_length = strlen(password->valuestring);
        if (ssid_length < 1 || ssid_length > 32 || password_length > 63 ||
                !valid_wifi_security(security->valuestring)) {
            return false;
        }
        output->action = TELEMETRY_WIFI_ACTION_CONNECT;
        snprintf(output->ssid, sizeof(output->ssid), "%s", ssid->valuestring);
        snprintf(output->password, sizeof(output->password), "%s",
                 password->valuestring);
        snprintf(output->security, sizeof(output->security), "%s",
                 security->valuestring);
        return true;
    }
    if (strcmp(action->valuestring, "select_uplink") == 0) {
        const cJSON *use_wifi = cJSON_GetObjectItemCaseSensitive(params, "use_wifi");
        if (!cJSON_IsBool(use_wifi)) {
            return false;
        }
        output->action = TELEMETRY_WIFI_ACTION_SELECT_UPLINK;
        output->use_wifi = cJSON_IsTrue(use_wifi);
        return true;
    }
    return false;
}

static bool parse_control_command(const char *payload, size_t length,
                                  control_command_t *output)
{
    cJSON *root = cJSON_ParseWithLengthOpts(payload, length + 1, NULL, true);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *packet_request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *boot_id = cJSON_GetObjectItemCaseSensitive(root, "boot_id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const bool common_valid = cJSON_IsString(protocol) &&
                              strcmp(protocol->valuestring, "ESCTL/1") == 0 &&
                              cJSON_IsString(kind) &&
                              strcmp(kind->valuestring, "command") == 0 &&
                              cJSON_IsString(command) &&
                              cJSON_IsString(packet_request_id) &&
                              valid_request_id(packet_request_id->valuestring) &&
                              cJSON_IsString(device_id) &&
                              strcmp(device_id->valuestring, CONFIG_TELEMETRY_DEVICE_ID) == 0 &&
                              cJSON_IsString(boot_id) &&
                              strcmp(boot_id->valuestring, s_boot_id) == 0 &&
                              cJSON_IsObject(params);
    if (!common_valid) {
        cJSON_Delete(root);
        return false;
    }

    snprintf(output->request_id, sizeof(output->request_id), "%s",
             packet_request_id->valuestring);
    if (strcmp(command->valuestring, "ap_stream") == 0) {
        const cJSON *packet_enabled =
            cJSON_GetObjectItemCaseSensitive(params, "enabled");
        if (!cJSON_IsBool(packet_enabled)) {
            cJSON_Delete(root);
            return false;
        }
        output->kind = CONTROL_COMMAND_AP_STREAM;
        output->data.ap_stream_enabled = cJSON_IsTrue(packet_enabled);
    } else if (strcmp(command->valuestring, "ps2_button") == 0) {
        const cJSON *button = cJSON_GetObjectItemCaseSensitive(params, "button");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(params, "state");
        if (!cJSON_IsString(button) || !valid_ps2_button(button->valuestring) ||
                !cJSON_IsString(state) || !valid_ps2_state(state->valuestring)) {
            cJSON_Delete(root);
            return false;
        }
        output->kind = CONTROL_COMMAND_PS2_BUTTON;
        snprintf(output->data.ps2.button, sizeof(output->data.ps2.button), "%s",
                 button->valuestring);
        snprintf(output->data.ps2.state, sizeof(output->data.ps2.state), "%s",
                 state->valuestring);
    } else if (strcmp(command->valuestring, "modem_4g") == 0) {
        output->kind = CONTROL_COMMAND_MODEM_4G;
        if (!parse_modem_command(params, &output->data.modem)) {
            cJSON_Delete(root);
            return false;
        }
        snprintf(output->data.modem.request_id,
                 sizeof(output->data.modem.request_id), "%s",
                 packet_request_id->valuestring);
    } else if (strcmp(command->valuestring, "video_fps") == 0) {
        const cJSON *fps = cJSON_GetObjectItemCaseSensitive(params, "fps");
        const cJSON *resolution =
            cJSON_GetObjectItemCaseSensitive(params, "resolution");
        if (!cJSON_IsNumber(fps) ||
                !(fps->valueint == 5 || fps->valueint == 8 ||
                  fps->valueint == 15 || fps->valueint == 20 ||
                  fps->valueint == 30) ||
                (resolution != NULL &&
                 (!cJSON_IsString(resolution) ||
                  !video_resolution_dimensions(resolution->valuestring,
                                               NULL, NULL)))) {
            cJSON_Delete(root);
            return false;
        }
        output->kind = CONTROL_COMMAND_VIDEO_FPS;
        output->data.video_fps.fps = fps->valueint;
        if (resolution != NULL) {
            output->data.video_fps.resolution_set = true;
            snprintf(output->data.video_fps.resolution,
                     sizeof(output->data.video_fps.resolution), "%s",
                     resolution->valuestring);
        }
    } else if (strcmp(command->valuestring, "wifi_sta") == 0) {
        output->kind = CONTROL_COMMAND_WIFI_STA;
        if (!parse_wifi_command(params, &output->data.wifi)) {
            cJSON_Delete(root);
            return false;
        }
        snprintf(output->data.wifi.request_id,
                 sizeof(output->data.wifi.request_id), "%s",
                 packet_request_id->valuestring);
    } else {
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);
    return true;
}

static bool send_ps2_uart_command(const control_command_t *command,
                                  uint32_t *sequence)
{
#if CONFIG_TELEMETRY_UART_SHARED_CONSOLE_TEST
    (void)command;
    (void)sequence;
    ESP_LOGW(TAG, "PS2 command ignored in UART0 shared-console test mode");
    return false;
#else
    char json[PS2_UART_JSON_MAX];
    const int json_length = snprintf(
        json, sizeof(json), "{\"button\":\"%s\",\"state\":\"%s\"}",
        command->data.ps2.button, command->data.ps2.state);
    if (json_length <= 0 || json_length >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "Failed to encode PS2 UART command");
        return false;
    }

    uint8_t frame[TELEMETRY_FIXED_HEADER + PS2_UART_JSON_MAX +
                  TELEMETRY_CRC_SIZE + TELEMETRY_TAIL_SIZE];
    const uint32_t frame_sequence = ++(*sequence);
    const size_t payload_length = (size_t)json_length;
    const size_t crc_offset = TELEMETRY_FIXED_HEADER + payload_length;
    const size_t tail_offset = crc_offset + TELEMETRY_CRC_SIZE;

    frame[0] = TELEMETRY_MAGIC_0;
    frame[1] = TELEMETRY_MAGIC_1;
    frame[2] = TELEMETRY_VERSION;
    frame[3] = TELEMETRY_TYPE_PS2_CONTROL;
    write_u16_be(frame + 4, (uint16_t)payload_length);
    write_u32_be(frame + 6, frame_sequence);
    memcpy(frame + TELEMETRY_FIXED_HEADER, json, payload_length);
    const uint16_t crc = crc16_ccitt_false(
        frame + 2, (TELEMETRY_FIXED_HEADER - 2) + payload_length);
    write_u16_be(frame + crc_offset, crc);
    frame[tail_offset] = 0x0D;
    frame[tail_offset + 1] = 0x0A;

    const size_t frame_length = tail_offset + TELEMETRY_TAIL_SIZE;
    const int written = uart_write_bytes(telemetry_uart_port(), frame, frame_length);
    if (written != (int)frame_length) {
        ESP_LOGE(TAG, "PS2 UART write failed: seq=%" PRIu32 " written=%d expected=%u",
                 frame_sequence, written, (unsigned)frame_length);
        return false;
    }
    ESP_LOGI(TAG, "PS2 UART command seq=%" PRIu32 " button=%s state=%s",
             frame_sequence, command->data.ps2.button, command->data.ps2.state);
    return true;
#endif
}

static bool send_unified_json(int fd, const uint8_t *payload, size_t payload_length, uint32_t sequence)
{
    cJSON *input = cJSON_ParseWithLength((const char *)payload, payload_length);
    if (input == NULL || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        ESP_LOGW(TAG, "Valid USART frame contains invalid JSON");
        return false;
    }
    cJSON *sensor = cJSON_GetObjectItemCaseSensitive(input, "sensor");
    cJSON *gps = cJSON_GetObjectItemCaseSensitive(input, "gps");
    if ((sensor != NULL && !cJSON_IsObject(sensor)) ||
            (gps != NULL && !cJSON_IsObject(gps))) {
        cJSON_Delete(input);
        ESP_LOGW(TAG, "USART sensor/gps fields must be JSON objects when present");
        return false;
    }

    cJSON *output = cJSON_CreateObject();
    if (output == NULL) {
        cJSON_Delete(input);
        return false;
    }
    cJSON_AddStringToObject(output, "protocol", "ESUT/1");
    cJSON_AddStringToObject(output, "device_id", CONFIG_TELEMETRY_DEVICE_ID);
    cJSON_AddStringToObject(output, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(output, "seq", sequence);
    cJSON_AddNumberToObject(output, "timestamp", (double)(esp_timer_get_time() / 1000));
    /* Missing group/field means NA. This lets STM32 report only the data it has. */
    cJSON_AddItemToObject(output, "sensor",
                          sensor != NULL ? cJSON_Duplicate(sensor, true) : cJSON_CreateObject());
    cJSON_AddItemToObject(output, "gps",
                          gps != NULL ? cJSON_Duplicate(gps, true) : cJSON_CreateObject());

    char *encoded = cJSON_PrintUnformatted(output);
    cJSON_Delete(output);
    cJSON_Delete(input);
    if (encoded == NULL) {
        return false;
    }

    size_t encoded_length = strlen(encoded);
    ssize_t sent = send(fd, encoded, encoded_length, 0);
    cJSON_free(encoded);
    if (sent != (ssize_t)encoded_length) {
        ESP_LOGW(TAG, "Telemetry UDP send failed: sent=%d expected=%u errno=%d",
                 (int)sent, (unsigned)encoded_length, errno);
        return false;
    }
    return true;
}

static bool send_heartbeat(int fd, uint32_t heartbeat_number)
{
    ap_stream_snapshot_t ap_stream = {0};
    modem_snapshot_t modem = {0};
    video_fps_snapshot_t video_fps = {0};
    wifi_snapshot_t wifi = {0};
    get_ap_stream_snapshot(&ap_stream);
    get_modem_snapshot(&modem);
    get_video_fps_snapshot(&video_fps);
    get_wifi_snapshot(&wifi);
    cJSON *heartbeat = cJSON_CreateObject();
    if (heartbeat == NULL) {
        return false;
    }
    cJSON_AddStringToObject(heartbeat, "protocol", "ESUT/1");
    cJSON_AddStringToObject(heartbeat, "kind", "heartbeat");
    cJSON_AddStringToObject(heartbeat, "device_id", CONFIG_TELEMETRY_DEVICE_ID);
    cJSON_AddStringToObject(heartbeat, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(heartbeat, "timestamp", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(heartbeat, "heartbeat_seq", heartbeat_number);
    cJSON *ap_stream_json = cJSON_AddObjectToObject(heartbeat, "ap_stream");
    if (ap_stream_json == NULL) {
        cJSON_Delete(heartbeat);
        return false;
    }
    cJSON_AddBoolToObject(ap_stream_json, "supported", ap_stream.supported);
    cJSON_AddStringToObject(ap_stream_json, "state",
                           ap_stream_state_name(ap_stream.state));
    if (ap_stream.request_id[0] != '\0') {
        cJSON_AddStringToObject(ap_stream_json, "request_id", ap_stream.request_id);
    } else {
        cJSON_AddNullToObject(ap_stream_json, "request_id");
    }
    if (ap_stream.error != ESP_OK) {
        cJSON_AddStringToObject(ap_stream_json, "error",
                               esp_err_to_name(ap_stream.error));
    } else {
        cJSON_AddNullToObject(ap_stream_json, "error");
    }
    cJSON *modem_json = cJSON_AddObjectToObject(heartbeat, "modem_4g");
    if (modem_json == NULL) {
        cJSON_Delete(heartbeat);
        return false;
    }
    cJSON_AddBoolToObject(modem_json, "supported", modem.supported);
    cJSON_AddStringToObject(modem_json, "state",
                           modem_state_name(modem.value.state));
    cJSON_AddStringToObject(modem_json, "action",
                           modem_action_name(modem.value.action));
    if (modem.value.request_id[0] != '\0') {
        cJSON_AddStringToObject(modem_json, "request_id", modem.value.request_id);
    } else {
        cJSON_AddNullToObject(modem_json, "request_id");
    }
    if (modem.value.error[0] != '\0') {
        cJSON_AddStringToObject(modem_json, "error", modem.value.error);
    } else {
        cJSON_AddNullToObject(modem_json, "error");
    }
    if (modem.value.operator_name[0] != '\0') {
        cJSON_AddStringToObject(modem_json, "operator", modem.value.operator_name);
    } else {
        cJSON_AddNullToObject(modem_json, "operator");
    }
    if (modem.value.registration >= 0) {
        cJSON_AddNumberToObject(modem_json, "registration", modem.value.registration);
    } else {
        cJSON_AddNullToObject(modem_json, "registration");
    }
    if (modem.value.rssi >= 0) {
        cJSON_AddNumberToObject(modem_json, "rssi", modem.value.rssi);
    } else {
        cJSON_AddNullToObject(modem_json, "rssi");
    }
    if (modem.value.rat[0] != '\0') {
        cJSON_AddStringToObject(modem_json, "rat", modem.value.rat);
    } else {
        cJSON_AddNullToObject(modem_json, "rat");
    }
    cJSON_AddStringToObject(modem_json, "band_config", modem.value.band_config);
    cJSON_AddStringToObject(modem_json, "cell_lock", modem.value.cell_lock_config);
    cJSON *cells = cJSON_AddArrayToObject(modem_json, "cells");
    if (cells == NULL) {
        cJSON_Delete(heartbeat);
        return false;
    }
    for (size_t i = 0; i < modem.value.cell_count &&
            i < TELEMETRY_MODEM_MAX_CELLS; ++i) {
        const telemetry_modem_cell_t *cell = &modem.value.cells[i];
        cJSON *cell_json = cJSON_CreateObject();
        if (cell_json == NULL || !cJSON_AddItemToArray(cells, cell_json)) {
            cJSON_Delete(cell_json);
            cJSON_Delete(heartbeat);
            return false;
        }
        cJSON_AddBoolToObject(cell_json, "serving", cell->serving);
        cJSON_AddNumberToObject(cell_json, "mcc", cell->mcc);
        cJSON_AddNumberToObject(cell_json, "mnc", cell->mnc);
        cJSON_AddStringToObject(cell_json, "tac", cell->tac);
        cJSON_AddStringToObject(cell_json, "cell_id", cell->cell_id);
        cJSON_AddNumberToObject(cell_json, "earfcn", cell->earfcn);
        cJSON_AddNumberToObject(cell_json, "pci", cell->pci);
        cJSON_AddNumberToObject(cell_json, "band", cell->band);
        cJSON_AddNumberToObject(cell_json, "rxlev", cell->rxlev);
        cJSON_AddNumberToObject(cell_json, "rsrp", cell->rsrp);
        cJSON_AddNumberToObject(cell_json, "rsrq", cell->rsrq);
    }
    cJSON *video_fps_json = cJSON_AddObjectToObject(heartbeat, "video_fps");
    if (video_fps_json == NULL) {
        cJSON_Delete(heartbeat);
        return false;
    }
    cJSON_AddBoolToObject(video_fps_json, "supported", video_fps.supported);
    cJSON_AddNumberToObject(video_fps_json, "fps", video_fps.fps);
    int video_width = 640;
    int video_height = 480;
    if (!video_resolution_dimensions(video_fps.resolution,
                                     &video_width, &video_height)) {
        snprintf(video_fps.resolution, sizeof(video_fps.resolution), "%s",
                 "640x480");
    }
    cJSON_AddStringToObject(video_fps_json, "resolution",
                           video_fps.resolution);
    cJSON_AddNumberToObject(video_fps_json, "width", video_width);
    cJSON_AddNumberToObject(video_fps_json, "height", video_height);
    if (video_fps.request_id[0] != '\0') {
        cJSON_AddStringToObject(video_fps_json, "request_id",
                               video_fps.request_id);
    } else {
        cJSON_AddNullToObject(video_fps_json, "request_id");
    }
    if (video_fps.error != ESP_OK) {
        cJSON_AddStringToObject(video_fps_json, "error",
                               esp_err_to_name(video_fps.error));
    } else {
        cJSON_AddNullToObject(video_fps_json, "error");
    }
    cJSON *wifi_json = cJSON_AddObjectToObject(heartbeat, "wifi_sta");
    if (wifi_json == NULL) {
        cJSON_Delete(heartbeat);
        return false;
    }
    cJSON_AddBoolToObject(wifi_json, "supported", wifi.supported);
    cJSON_AddStringToObject(wifi_json, "state",
                            wifi_state_name(wifi.value.state));
    cJSON_AddStringToObject(wifi_json, "action",
                            wifi_action_name(wifi.value.action));
    if (wifi.value.request_id[0] != '\0') {
        cJSON_AddStringToObject(wifi_json, "request_id",
                                wifi.value.request_id);
    } else {
        cJSON_AddNullToObject(wifi_json, "request_id");
    }
    if (wifi.value.error[0] != '\0') {
        cJSON_AddStringToObject(wifi_json, "error", wifi.value.error);
    } else {
        cJSON_AddNullToObject(wifi_json, "error");
    }
    cJSON_AddBoolToObject(wifi_json, "feature_enabled",
                          wifi.value.feature_enabled);
    cJSON_AddBoolToObject(wifi_json, "scanning", wifi.value.scanning);
    cJSON_AddBoolToObject(wifi_json, "connected", wifi.value.connected);
    cJSON_AddStringToObject(wifi_json, "ssid", wifi.value.ssid);
    cJSON_AddStringToObject(wifi_json, "ip", wifi.value.ip);
    if (wifi.value.rssi_valid) {
        cJSON_AddNumberToObject(wifi_json, "rssi", wifi.value.rssi);
    } else {
        cJSON_AddNullToObject(wifi_json, "rssi");
    }
    cJSON_AddBoolToObject(wifi_json, "wifi_uplink_selected",
                          wifi.value.wifi_uplink_selected);
    cJSON_AddStringToObject(wifi_json, "active_uplink",
                            wifi.value.active_uplink[0] != '\0'
                                ? wifi.value.active_uplink : "none");
    cJSON *networks = cJSON_AddArrayToObject(wifi_json, "networks");
    if (networks == NULL) {
        cJSON_Delete(heartbeat);
        return false;
    }
    for (size_t i = 0; i < wifi.value.network_count &&
            i < TELEMETRY_WIFI_MAX_NETWORKS; ++i) {
        const telemetry_wifi_network_t *network = &wifi.value.networks[i];
        cJSON *network_json = cJSON_CreateObject();
        if (network_json == NULL ||
                !cJSON_AddItemToArray(networks, network_json)) {
            cJSON_Delete(network_json);
            cJSON_Delete(heartbeat);
            return false;
        }
        cJSON_AddStringToObject(network_json, "ssid", network->ssid);
        cJSON_AddNumberToObject(network_json, "rssi", network->rssi);
        cJSON_AddNumberToObject(network_json, "channel", network->channel);
        cJSON_AddStringToObject(network_json, "security", network->security);
        cJSON_AddBoolToObject(network_json, "secured", network->secured);
        cJSON_AddBoolToObject(network_json, "supported", network->supported);
    }
    char *encoded = cJSON_PrintUnformatted(heartbeat);
    cJSON_Delete(heartbeat);
    if (encoded == NULL) {
        return false;
    }
    const size_t length = strlen(encoded);
    const ssize_t sent = send(fd, encoded, length, 0);
    cJSON_free(encoded);
    if (sent != (ssize_t)length) {
        ESP_LOGW(TAG, "ESP heartbeat UDP send failed: sent=%d expected=%u errno=%d",
                 (int)sent, (unsigned)length, errno);
        return false;
    }
    if (heartbeat_number == 1 || heartbeat_number % 30 == 0) {
        ESP_LOGI(TAG, "ESP online heartbeat #%" PRIu32, heartbeat_number);
    }
    return true;
}

static bool receive_control_commands(int fd, uint32_t *heartbeat_number,
                                     uint32_t *ps2_sequence)
{
    char payload[CONTROL_MAX_JSON + 1];
    for (int packet = 0; packet < 4; ++packet) {
        ssize_t received = recv(fd, payload, sizeof(payload), MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            ESP_LOGW(TAG, "Control UDP receive failed: errno=%d", errno);
            return false;
        }
        if (received == 0) {
            return true;
        }
        if (received > CONTROL_MAX_JSON) {
            ESP_LOGW(TAG, "Rejected oversized ESCTL/1 control packet");
            continue;
        }
        payload[received] = '\0';

        control_command_t command = {0};
        if (!parse_control_command(payload, (size_t)received, &command)) {
            ESP_LOGW(TAG, "Rejected invalid ESCTL/1 control packet (%d bytes)",
                     (int)received);
            continue;
        }

        if (command.kind == CONTROL_COMMAND_PS2_BUTTON) {
            send_ps2_uart_command(&command, ps2_sequence);
            continue;
        }

        if (command.kind == CONTROL_COMMAND_WIFI_STA) {
            const telemetry_wifi_command_t *wifi_command = &command.data.wifi;
            wifi_snapshot_t current = {0};
            get_wifi_snapshot(&current);
            if (current.value.request_id[0] != '\0' &&
                    strcmp(current.value.request_id,
                           wifi_command->request_id) == 0) {
                if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                    return false;
                }
                continue;
            }
            if (!current.supported || current.control_cb == NULL) {
                wifi_set_command_state(wifi_command, TELEMETRY_WIFI_ERROR,
                                       "WIFI_CONTROL_UNSUPPORTED");
                if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                    return false;
                }
                continue;
            }
            wifi_set_command_state(wifi_command, TELEMETRY_WIFI_APPLYING, NULL);
            esp_err_t queued = current.control_cb(wifi_command,
                                                   current.control_ctx);
            if (queued != ESP_OK) {
                wifi_set_command_state(wifi_command, TELEMETRY_WIFI_ERROR,
                                       esp_err_to_name(queued));
            }
            if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                return false;
            }
            ESP_LOGI(TAG, "wifi_sta request=%s action=%s result=%s",
                     wifi_command->request_id,
                     wifi_action_name(wifi_command->action),
                     queued == ESP_OK ? "queued" : esp_err_to_name(queued));
            continue;
        }

        if (command.kind == CONTROL_COMMAND_VIDEO_FPS) {
            video_fps_snapshot_t current = {0};
            get_video_fps_snapshot(&current);
            if (current.request_id[0] != '\0' &&
                    strcmp(current.request_id, command.request_id) == 0) {
                if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                    return false;
                }
                continue;
            }
            const char *requested_resolution =
                command.data.video_fps.resolution_set
                    ? command.data.video_fps.resolution : NULL;
            esp_err_t ret = (!current.supported || current.control_cb == NULL)
                ? ESP_ERR_NOT_SUPPORTED
                : current.control_cb(command.data.video_fps.fps,
                                     requested_resolution,
                                     current.control_ctx);
            set_video_fps_state(command.request_id,
                                command.data.video_fps.fps,
                                requested_resolution, ret);
            if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                return false;
            }
            ESP_LOGI(TAG,
                     "video_fps request=%s fps=%d resolution=%s result=%s",
                     command.request_id, command.data.video_fps.fps,
                     requested_resolution == NULL
                         ? current.resolution : requested_resolution,
                     ret == ESP_OK ? "applied" : esp_err_to_name(ret));
            continue;
        }

        if (command.kind == CONTROL_COMMAND_MODEM_4G) {
            const telemetry_modem_command_t *modem_command = &command.data.modem;
            modem_snapshot_t current = {0};
            get_modem_snapshot(&current);
            if (current.value.request_id[0] != '\0' &&
                    strcmp(current.value.request_id, modem_command->request_id) == 0) {
                if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                    return false;
                }
                continue;
            }
            if (modem_command->action == TELEMETRY_MODEM_ACTION_PING) {
                modem_set_command_state(
                    modem_command,
                    current.supported ? TELEMETRY_MODEM_SUCCESS
                                      : TELEMETRY_MODEM_ERROR,
                    current.supported ? NULL : "MODEM_CONTROL_UNSUPPORTED");
                if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                    return false;
                }
                continue;
            }
            if (!current.supported || current.control_cb == NULL) {
                modem_set_command_state(modem_command, TELEMETRY_MODEM_ERROR,
                                        "MODEM_CONTROL_UNSUPPORTED");
                if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                    return false;
                }
                continue;
            }
            modem_set_command_state(
                modem_command,
                modem_command->action == TELEMETRY_MODEM_ACTION_QUERY
                    ? TELEMETRY_MODEM_QUERYING : TELEMETRY_MODEM_APPLYING,
                NULL);
            esp_err_t queued = current.control_cb(modem_command, current.control_ctx);
            if (queued != ESP_OK) {
                modem_set_command_state(modem_command, TELEMETRY_MODEM_ERROR,
                                        esp_err_to_name(queued));
            }
            if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                return false;
            }
            ESP_LOGI(TAG, "modem_4g request=%s action=%s result=%s",
                     modem_command->request_id,
                     modem_action_name(modem_command->action),
                     queued == ESP_OK ? "queued" : esp_err_to_name(queued));
            continue;
        }

        const char *request_id = command.request_id;
        const bool enabled = command.data.ap_stream_enabled;

        ap_stream_snapshot_t current = {0};
        get_ap_stream_snapshot(&current);
        if (current.last_command_valid &&
                strcmp(current.request_id, request_id) == 0) {
            if (current.last_command_enabled != enabled) {
                ESP_LOGW(TAG, "Rejected reused request_id with different ap_stream value");
                continue;
            }
            /* UDP retries are idempotent: report the current state without toggling again. */
            if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                return false;
            }
            continue;
        }

        if (!current.supported || current.control_cb == NULL) {
            ap_stream_set_command_state(request_id, enabled,
                                        TELEMETRY_AP_STREAM_UNSUPPORTED,
                                        ESP_ERR_NOT_SUPPORTED);
            if (!send_heartbeat(fd, ++(*heartbeat_number))) {
                return false;
            }
            continue;
        }

        ap_stream_set_command_state(
            request_id, enabled,
            enabled ? TELEMETRY_AP_STREAM_STARTING : TELEMETRY_AP_STREAM_STOPPING,
            ESP_OK);
        bool socket_ok = send_heartbeat(fd, ++(*heartbeat_number));

        ESP_LOGI(TAG, "Applying ap_stream request=%s enabled=%s", request_id,
                 enabled ? "true" : "false");
        esp_err_t ret = current.control_cb(enabled, current.control_ctx);
        ap_stream_set_command_state(
            request_id, enabled,
            ret == ESP_OK
                ? (enabled ? TELEMETRY_AP_STREAM_ENABLED : TELEMETRY_AP_STREAM_DISABLED)
                : TELEMETRY_AP_STREAM_ERROR,
            ret);
        if (!socket_ok || !send_heartbeat(fd, ++(*heartbeat_number))) {
            return false;
        }
        ESP_LOGI(TAG, "ap_stream request=%s result=%s", request_id,
                 ret == ESP_OK ? (enabled ? "enabled" : "disabled")
                               : esp_err_to_name(ret));
    }
    return true;
}

static bool validate_and_send(frame_parser_t *parser, int fd)
{
    const uint16_t payload_length = read_u16_be(parser->data + 4);
    const size_t crc_offset = TELEMETRY_FIXED_HEADER + payload_length;
    const size_t tail_offset = crc_offset + TELEMETRY_CRC_SIZE;

    if (parser->data[2] != TELEMETRY_VERSION ||
            parser->data[3] != TELEMETRY_TYPE_SENSOR_GPS) {
        parser_drop(parser, "unsupported version/type");
        return false;
    }
    if (parser->data[tail_offset] != 0x0D || parser->data[tail_offset + 1] != 0x0A) {
        parser->tail_errors++;
        parser_drop(parser, "missing frame tail");
        return false;
    }

    const uint16_t expected_crc = read_u16_be(parser->data + crc_offset);
    const uint16_t actual_crc = crc16_ccitt_false(
        parser->data + 2, (TELEMETRY_FIXED_HEADER - 2) + payload_length);
    if (expected_crc != actual_crc) {
        parser->crc_errors++;
        parser_drop(parser, "CRC16 mismatch");
        return false;
    }

    uint32_t sequence = read_u32_be(parser->data + 6);
    bool accepted = send_unified_json(fd, parser->data + TELEMETRY_FIXED_HEADER,
                                     payload_length, sequence);
    if (accepted) {
        parser->valid_frames++;
        if (parser->valid_frames <= 3 || parser->valid_frames % 50 == 0) {
            ESP_LOGI(TAG, "Complete USART telemetry frame #%" PRIu32 " seq=%" PRIu32,
                     parser->valid_frames, sequence);
        }
    } else {
        parser->dropped_frames++;
    }
    parser_reset(parser);
    return accepted;
}

static void parser_check_timeout(frame_parser_t *parser)
{
    if (parser->used < 2 || parser->started_us == 0) {
        return;
    }
    if (esp_timer_get_time() - parser->started_us >
            (int64_t)CONFIG_TELEMETRY_UART_FRAME_TIMEOUT_MS * 1000) {
        parser->timeout_errors++;
        parser_drop(parser, "header received but frame timed out");
    }
}

static void parser_feed(frame_parser_t *parser, uint8_t byte, int fd)
{
    parser_check_timeout(parser);

    if (parser->used == 0) {
        if (byte == TELEMETRY_MAGIC_0) {
            parser->data[0] = byte;
            parser->used = 1;
        }
        return;
    }
    if (parser->used == 1) {
        if (byte == TELEMETRY_MAGIC_1) {
            parser->data[1] = byte;
            parser->used = 2;
            parser->started_us = esp_timer_get_time();
        } else if (byte != TELEMETRY_MAGIC_0) {
            parser->used = 0;
        }
        return;
    }

    if (parser->used >= sizeof(parser->data)) {
        parser_drop(parser, "frame buffer overflow");
        return;
    }
    parser->data[parser->used++] = byte;

    if (parser->used == 6) {
        uint16_t payload_length = read_u16_be(parser->data + 4);
        if (payload_length == 0 || payload_length > TELEMETRY_MAX_JSON) {
            parser_drop(parser, "payload length out of range");
            return;
        }
        parser->expected = TELEMETRY_FIXED_HEADER + payload_length +
                           TELEMETRY_CRC_SIZE + TELEMETRY_TAIL_SIZE;
    }
    if (parser->expected > 0 && parser->used == parser->expected) {
        if (s_network_ready && fd >= 0) {
            validate_and_send(parser, fd);
        } else {
            /* Still validate framing so diagnostics distinguish UART from network loss. */
            const uint16_t payload_length = read_u16_be(parser->data + 4);
            const size_t crc_offset = TELEMETRY_FIXED_HEADER + payload_length;
            const size_t tail_offset = crc_offset + TELEMETRY_CRC_SIZE;
            const uint16_t actual_crc = crc16_ccitt_false(
                parser->data + 2, (TELEMETRY_FIXED_HEADER - 2) + payload_length);
            if (parser->data[tail_offset] == 0x0D && parser->data[tail_offset + 1] == 0x0A &&
                    read_u16_be(parser->data + crc_offset) == actual_crc) {
                parser->valid_frames++;
            } else {
                parser->dropped_frames++;
            }
            parser_reset(parser);
        }
    }
}

static void telemetry_task(void *argument)
{
    (void)argument;
    frame_parser_t parser = {0};
    uint8_t rx[128];
    int udp_fd = -1;
    int64_t next_heartbeat_us = 0;
    uint32_t heartbeat_number = 0;
    uint32_t ps2_sequence = 0;
    uint32_t active_uplink_generation = s_uplink_generation;

    while (true) {
        const uint32_t uplink_generation = s_uplink_generation;
        if (uplink_generation != active_uplink_generation) {
            if (udp_fd >= 0) {
                close(udp_fd);
                udp_fd = -1;
            }
            active_uplink_generation = uplink_generation;
            next_heartbeat_us = 0;
            ESP_LOGI(TAG, "Telemetry UDP socket reopen requested for uplink generation %lu",
                     (unsigned long)uplink_generation);
        }
        if (s_network_ready && udp_fd < 0) {
            udp_fd = open_udp_socket();
            if (udp_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else if (!s_network_ready && udp_fd >= 0) {
            close(udp_fd);
            udp_fd = -1;
            next_heartbeat_us = 0;
        }

        const int64_t now_us = esp_timer_get_time();
        if (udp_fd >= 0 && now_us >= next_heartbeat_us) {
            if (!send_heartbeat(udp_fd, ++heartbeat_number)) {
                close(udp_fd);
                udp_fd = -1;
                next_heartbeat_us = 0;
            } else {
                next_heartbeat_us = now_us +
                    (int64_t)CONFIG_TELEMETRY_HEARTBEAT_INTERVAL_MS * 1000;
            }
        }

        if (udp_fd >= 0 &&
                !receive_control_commands(udp_fd, &heartbeat_number, &ps2_sequence)) {
            close(udp_fd);
            udp_fd = -1;
            next_heartbeat_us = 0;
        }

        int count = uart_read_bytes(telemetry_uart_port(), rx, sizeof(rx),
                                    pdMS_TO_TICKS(20));
        for (int i = 0; i < count; ++i) {
            parser_feed(&parser, rx[i], udp_fd);
        }
        parser_check_timeout(&parser);
    }
}

void telemetry_uart_configure_ap_stream(bool supported,
                                        telemetry_ap_stream_state_t initial_state,
                                        telemetry_ap_stream_control_cb_t control_cb,
                                        void *control_ctx)
{
    portENTER_CRITICAL(&s_ap_stream_mux);
    s_ap_stream_supported = supported;
    s_ap_stream_state = supported ? initial_state : TELEMETRY_AP_STREAM_UNSUPPORTED;
    s_ap_stream_error = ESP_OK;
    s_ap_stream_request_id[0] = '\0';
    s_ap_stream_last_command_valid = false;
    s_ap_stream_control_cb = supported ? control_cb : NULL;
    s_ap_stream_control_ctx = supported ? control_ctx : NULL;
    portEXIT_CRITICAL(&s_ap_stream_mux);
}

void telemetry_uart_set_ap_stream_state(telemetry_ap_stream_state_t state,
                                        esp_err_t error)
{
    portENTER_CRITICAL(&s_ap_stream_mux);
    s_ap_stream_state = s_ap_stream_supported ? state : TELEMETRY_AP_STREAM_UNSUPPORTED;
    s_ap_stream_error = error;
    portEXIT_CRITICAL(&s_ap_stream_mux);
}

void telemetry_uart_configure_video_fps(bool supported, int initial_fps,
                                        const char *initial_resolution,
                                        telemetry_video_fps_control_cb_t control_cb,
                                        void *control_ctx)
{
    portENTER_CRITICAL(&s_video_fps_mux);
    s_video_fps_supported = supported;
    s_video_fps = initial_fps;
    snprintf(s_video_resolution, sizeof(s_video_resolution), "%s",
             video_resolution_dimensions(initial_resolution, NULL, NULL)
                 ? initial_resolution : "640x480");
    s_video_fps_request_id[0] = '\0';
    s_video_fps_error = ESP_OK;
    s_video_fps_control_cb = supported ? control_cb : NULL;
    s_video_fps_control_ctx = supported ? control_ctx : NULL;
    portEXIT_CRITICAL(&s_video_fps_mux);
}

void telemetry_uart_configure_modem(bool supported,
                                    telemetry_modem_control_cb_t control_cb,
                                    void *control_ctx)
{
    portENTER_CRITICAL(&s_modem_mux);
    s_modem_supported = supported;
    memset(&s_modem_state, 0, sizeof(s_modem_state));
    s_modem_state.state = supported ? TELEMETRY_MODEM_IDLE
                                    : TELEMETRY_MODEM_UNSUPPORTED;
    s_modem_state.registration = -1;
    s_modem_state.rssi = -1;
    s_modem_control_cb = supported ? control_cb : NULL;
    s_modem_control_ctx = supported ? control_ctx : NULL;
    portEXIT_CRITICAL(&s_modem_mux);
}

void telemetry_uart_set_modem_state(const telemetry_modem_state_t *state)
{
    if (state == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_modem_mux);
    if (s_modem_supported) {
        s_modem_state = *state;
        if (s_modem_state.cell_count > TELEMETRY_MODEM_MAX_CELLS) {
            s_modem_state.cell_count = TELEMETRY_MODEM_MAX_CELLS;
        }
    }
    portEXIT_CRITICAL(&s_modem_mux);
}

void telemetry_uart_configure_wifi(bool supported,
                                   telemetry_wifi_control_cb_t control_cb,
                                   void *control_ctx)
{
    portENTER_CRITICAL(&s_wifi_mux);
    s_wifi_supported = supported;
    memset(&s_wifi_state, 0, sizeof(s_wifi_state));
    s_wifi_state.state = TELEMETRY_WIFI_IDLE;
    s_wifi_state.action = TELEMETRY_WIFI_ACTION_QUERY;
    snprintf(s_wifi_state.active_uplink,
             sizeof(s_wifi_state.active_uplink), "none");
    s_wifi_control_cb = supported ? control_cb : NULL;
    s_wifi_control_ctx = supported ? control_ctx : NULL;
    portEXIT_CRITICAL(&s_wifi_mux);
}

void telemetry_uart_set_wifi_state(const telemetry_wifi_state_t *state)
{
    if (state == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_wifi_mux);
    if (s_wifi_supported) {
        s_wifi_state = *state;
        if (s_wifi_state.network_count > TELEMETRY_WIFI_MAX_NETWORKS) {
            s_wifi_state.network_count = TELEMETRY_WIFI_MAX_NETWORKS;
        }
        /* Passwords exist only in telemetry_wifi_command_t.  This state type
         * deliberately cannot retain or publish credentials. */
    }
    portEXIT_CRITICAL(&s_wifi_mux);
}

esp_err_t telemetry_uart_start(void)
{
#if !CONFIG_TELEMETRY_UART_ENABLE
    ESP_LOGI(TAG, "Unified USART telemetry disabled");
    return ESP_OK;
#else
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_boot_id, sizeof(s_boot_id), "%08" PRIX32, esp_random());
    uart_config_t uart_config = {
        .baud_rate = CONFIG_TELEMETRY_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_port_t port = telemetry_uart_port();
    ESP_RETURN_ON_ERROR(uart_driver_install(port, 4096, 0, 0, NULL, 0), TAG,
                        "UART driver install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(port, &uart_config), TAG,
                        "UART configuration failed");
#if !CONFIG_TELEMETRY_UART_SHARED_CONSOLE_TEST
    ESP_RETURN_ON_ERROR(
        uart_set_pin(port, CONFIG_TELEMETRY_UART_TX_GPIO, CONFIG_TELEMETRY_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "UART pin configuration failed");
#endif

    /* Control parsing and heartbeat snapshots now include Wi-Fi scan results;
     * retain headroom for the receive -> heartbeat -> cJSON call chain. */
    BaseType_t created = xTaskCreatePinnedToCore(telemetry_task, "telemetry_uart", 8192,
                                                 NULL, 9, NULL, 1);
    if (created != pdPASS) {
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
#if CONFIG_TELEMETRY_UART_SHARED_CONSOLE_TEST
    ESP_LOGW(TAG, "UART0 shared-console test mode: RX uses existing console pins, baud=%d boot=%s",
             CONFIG_TELEMETRY_UART_BAUD, s_boot_id);
#else
    ESP_LOGI(TAG, "USART framed telemetry ready: UART%d RX=GPIO%d TX=GPIO%d baud=%d boot=%s",
             CONFIG_TELEMETRY_UART_NUM, CONFIG_TELEMETRY_UART_RX_GPIO,
             CONFIG_TELEMETRY_UART_TX_GPIO, CONFIG_TELEMETRY_UART_BAUD, s_boot_id);
#endif
    return ESP_OK;
#endif
}

void telemetry_uart_set_network_ready(bool ready)
{
    s_network_ready = ready;
}

void telemetry_uart_set_uplink_interface(const char *ifname)
{
    snprintf(s_uplink_ifname, sizeof(s_uplink_ifname), "%s",
             ifname == NULL ? "" : ifname);
    ++s_uplink_generation;
    ESP_LOGI(TAG, "Telemetry UDP uplink interface=%s",
             s_uplink_ifname[0] == '\0' ? "default" : s_uplink_ifname);
}
