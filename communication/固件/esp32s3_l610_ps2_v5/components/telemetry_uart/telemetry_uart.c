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

typedef enum {
    CONTROL_COMMAND_AP_STREAM,
    CONTROL_COMMAND_PS2_BUTTON,
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
    } data;
} control_command_t;

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
    get_ap_stream_snapshot(&ap_stream);
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

    while (true) {
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

    BaseType_t created = xTaskCreatePinnedToCore(telemetry_task, "telemetry_uart", 6144,
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
    ESP_LOGI(TAG, "Telemetry UDP uplink interface=%s",
             s_uplink_ifname[0] == '\0' ? "default" : s_uplink_ifname);
}
