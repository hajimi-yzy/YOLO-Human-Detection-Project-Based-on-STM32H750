#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t camera_stream_start(void);
void camera_stream_set_network_ready(bool ready);
void camera_stream_set_uplink_interface(const char *ifname);
esp_err_t camera_stream_set_local_mjpeg_enabled(bool enabled);
bool camera_stream_local_mjpeg_is_enabled(void);
