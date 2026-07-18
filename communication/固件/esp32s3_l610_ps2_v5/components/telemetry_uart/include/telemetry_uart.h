#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    TELEMETRY_AP_STREAM_UNSUPPORTED = 0,
    TELEMETRY_AP_STREAM_STARTING,
    TELEMETRY_AP_STREAM_ENABLED,
    TELEMETRY_AP_STREAM_STOPPING,
    TELEMETRY_AP_STREAM_DISABLED,
    TELEMETRY_AP_STREAM_ERROR,
} telemetry_ap_stream_state_t;

typedef esp_err_t (*telemetry_ap_stream_control_cb_t)(bool enabled, void *ctx);
#define TELEMETRY_VIDEO_RESOLUTION_MAX 11

typedef esp_err_t (*telemetry_video_fps_control_cb_t)(
    int fps, const char *resolution, void *ctx);

#define TELEMETRY_MODEM_MAX_BANDS 10
#define TELEMETRY_MODEM_MAX_CELLS 6
#define TELEMETRY_MODEM_REQUEST_ID_MAX 64

typedef enum {
    TELEMETRY_MODEM_ACTION_QUERY = 0,
    TELEMETRY_MODEM_ACTION_PING,
    TELEMETRY_MODEM_ACTION_RESELECT,
    TELEMETRY_MODEM_ACTION_SET_BANDS,
    TELEMETRY_MODEM_ACTION_SET_CELL_LOCK,
    TELEMETRY_MODEM_ACTION_CLEAR_CELL_LOCK,
} telemetry_modem_action_t;

typedef struct {
    telemetry_modem_action_t action;
    char request_id[TELEMETRY_MODEM_REQUEST_ID_MAX + 1];
    uint16_t bands[TELEMETRY_MODEM_MAX_BANDS];
    size_t band_count;
    uint32_t earfcn;
    int pci;
    bool lock_pci;
} telemetry_modem_command_t;

typedef enum {
    TELEMETRY_MODEM_UNSUPPORTED = 0,
    TELEMETRY_MODEM_IDLE,
    TELEMETRY_MODEM_QUERYING,
    TELEMETRY_MODEM_APPLYING,
    TELEMETRY_MODEM_SUCCESS,
    TELEMETRY_MODEM_ERROR,
} telemetry_modem_state_code_t;

typedef struct {
    bool serving;
    int mcc;
    int mnc;
    char tac[12];
    char cell_id[16];
    uint32_t earfcn;
    int pci;
    int band;
    int rxlev;
    int rsrp;
    int rsrq;
} telemetry_modem_cell_t;

typedef struct {
    telemetry_modem_state_code_t state;
    char request_id[TELEMETRY_MODEM_REQUEST_ID_MAX + 1];
    telemetry_modem_action_t action;
    char error[64];
    char operator_name[48];
    int registration;
    int rssi;
    char rat[16];
    char band_config[128];
    char cell_lock_config[96];
    telemetry_modem_cell_t cells[TELEMETRY_MODEM_MAX_CELLS];
    size_t cell_count;
} telemetry_modem_state_t;

typedef esp_err_t (*telemetry_modem_control_cb_t)(
    const telemetry_modem_command_t *command, void *ctx);

#define TELEMETRY_WIFI_MAX_NETWORKS 10
#define TELEMETRY_WIFI_REQUEST_ID_MAX 64

typedef enum {
    TELEMETRY_WIFI_ACTION_QUERY = 0,
    TELEMETRY_WIFI_ACTION_SET_ENABLED,
    TELEMETRY_WIFI_ACTION_SCAN,
    TELEMETRY_WIFI_ACTION_CONNECT,
    TELEMETRY_WIFI_ACTION_SELECT_UPLINK,
} telemetry_wifi_action_t;

typedef enum {
    TELEMETRY_WIFI_IDLE = 0,
    TELEMETRY_WIFI_APPLYING,
    TELEMETRY_WIFI_SUCCESS,
    TELEMETRY_WIFI_ERROR,
} telemetry_wifi_state_code_t;

typedef struct {
    telemetry_wifi_action_t action;
    char request_id[TELEMETRY_WIFI_REQUEST_ID_MAX + 1];
    bool enabled;
    bool use_wifi;
    char ssid[33];
    char password[64];
    char security[16];
} telemetry_wifi_command_t;

typedef struct {
    char ssid[33];
    int rssi;
    int channel;
    char security[16];
    bool secured;
    bool supported;
} telemetry_wifi_network_t;

typedef struct {
    telemetry_wifi_state_code_t state;
    telemetry_wifi_action_t action;
    char request_id[TELEMETRY_WIFI_REQUEST_ID_MAX + 1];
    char error[64];
    bool feature_enabled;
    bool scanning;
    bool connected;
    char ssid[33];
    char ip[16];
    bool rssi_valid;
    int rssi;
    bool wifi_uplink_selected;
    char active_uplink[6];
    telemetry_wifi_network_t networks[TELEMETRY_WIFI_MAX_NETWORKS];
    size_t network_count;
} telemetry_wifi_state_t;

typedef esp_err_t (*telemetry_wifi_control_cb_t)(
    const telemetry_wifi_command_t *command, void *ctx);

void telemetry_uart_configure_ap_stream(bool supported,
                                        telemetry_ap_stream_state_t initial_state,
                                        telemetry_ap_stream_control_cb_t control_cb,
                                        void *control_ctx);
void telemetry_uart_set_ap_stream_state(telemetry_ap_stream_state_t state,
                                        esp_err_t error);
void telemetry_uart_configure_video_fps(bool supported, int initial_fps,
                                        const char *initial_resolution,
                                        telemetry_video_fps_control_cb_t control_cb,
                                        void *control_ctx);
void telemetry_uart_configure_modem(bool supported,
                                    telemetry_modem_control_cb_t control_cb,
                                    void *control_ctx);
void telemetry_uart_set_modem_state(const telemetry_modem_state_t *state);
void telemetry_uart_configure_wifi(bool supported,
                                   telemetry_wifi_control_cb_t control_cb,
                                   void *control_ctx);
void telemetry_uart_set_wifi_state(const telemetry_wifi_state_t *state);
esp_err_t telemetry_uart_start(void);
void telemetry_uart_set_network_ready(bool ready);
void telemetry_uart_set_uplink_interface(const char *ifname);
