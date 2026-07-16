idf_component_register(SRCS "usb_rndis_4g_module.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES camera_stream
                                  telemetry_uart
                                  nvs_flash
                                  esp_netif
                                  esp_event
                                  mbedtls
                                  espressif__iot_eth
                                  iot_usbh_rndis
                                  espressif__iot_usbh_cdc
                                  espressif__modem_at
                                  web_wifi)
