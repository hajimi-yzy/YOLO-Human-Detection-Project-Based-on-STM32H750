#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t camera_stream_start(void);
void camera_stream_set_network_ready(bool ready);
void camera_stream_set_uplink_interface(const char *ifname);
esp_err_t camera_stream_set_local_mjpeg_enabled(bool enabled);
bool camera_stream_local_mjpeg_is_enabled(void);
esp_err_t camera_stream_set_target_fps(int fps);
int camera_stream_get_target_fps(void);
esp_err_t camera_stream_set_video_settings(int fps, const char *resolution);
const char *camera_stream_get_resolution(void);
int camera_stream_get_width(void);
int camera_stream_get_height(void);
