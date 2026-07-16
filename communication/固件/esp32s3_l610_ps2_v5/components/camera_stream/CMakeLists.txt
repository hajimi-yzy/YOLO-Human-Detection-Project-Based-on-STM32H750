idf_component_register(SRCS "camera_stream.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp32-camera esp_timer esp_psram esp_rom esp_hw_support
                                esp_http_server lwip)
