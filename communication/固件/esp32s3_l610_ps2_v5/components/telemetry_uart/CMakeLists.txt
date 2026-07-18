idf_component_register(
    SRCS "telemetry_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer esp_system json lwip
)
