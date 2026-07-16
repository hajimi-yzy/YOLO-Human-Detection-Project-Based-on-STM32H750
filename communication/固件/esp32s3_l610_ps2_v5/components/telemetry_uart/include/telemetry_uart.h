#pragma once

#include <stdbool.h>
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

void telemetry_uart_configure_ap_stream(bool supported,
                                        telemetry_ap_stream_state_t initial_state,
                                        telemetry_ap_stream_control_cb_t control_cb,
                                        void *control_ctx);
void telemetry_uart_set_ap_stream_state(telemetry_ap_stream_state_t state,
                                        esp_err_t error);
esp_err_t telemetry_uart_start(void);
void telemetry_uart_set_network_ready(bool ready);
void telemetry_uart_set_uplink_interface(const char *ifname);
