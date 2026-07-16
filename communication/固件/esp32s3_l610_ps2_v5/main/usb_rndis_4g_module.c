/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "iot_usbh_rndis.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "at_3gpp_ts_27_007.h"
#include "camera_stream.h"
#include "telemetry_uart.h"
#ifdef CONFIG_L610_SOFTAP_SHARE_ENABLE
#include "app_wifi.h"
#endif

static const char *TAG = "RNDIS_4G_MODULE";

static EventGroupHandle_t s_event_group;
static iot_eth_driver_t *s_rndis_eth_driver = NULL;
static esp_netif_t *s_rndis_netif = NULL;
static char s_rndis_dns[16] = "unknown";
static bool s_camera_task_started = false;
static bool s_telemetry_task_started = false;
#ifdef CONFIG_L610_SOFTAP_SHARE_ENABLE
static modem_wifi_config_t s_modem_wifi_config = MODEM_WIFI_DEFAULT_CONFIG();
static volatile bool s_ap_stream_requested = true;
#endif

#define L610_USB_VID 0x1782
#define L610_RNDIS_PID 0x4D11

#define EVENT_GOT_IP_BIT (BIT0)
#define EVENT_AT_READY_BIT (BIT1)

#ifdef CONFIG_L610_SOFTAP_SHARE_ENABLE
static esp_err_t set_ap_stream_enabled(bool enabled, void *ctx)
{
    (void)ctx;
    if (s_rndis_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Pin the cloud uplink before changing Wi-Fi state and restore it even if
     * the AP operation fails. SoftAP is a LAN, never the upstream route. */
    esp_err_t route_before = esp_netif_set_default_netif(s_rndis_netif);
    if (route_before != ESP_OK) {
        ESP_LOGE(TAG, "Cannot preserve RNDIS route before AP transition: %s",
                 esp_err_to_name(route_before));
        return route_before;
    }

    esp_err_t operation;
    if (enabled) {
        operation = app_wifi_start_softap(&s_modem_wifi_config);
        if (operation == ESP_OK) {
            operation = camera_stream_set_local_mjpeg_enabled(true);
            if (operation != ESP_OK) {
                ESP_LOGE(TAG, "Local MJPEG start failed; rolling back SoftAP: %s",
                         esp_err_to_name(operation));
                app_wifi_stop_softap();
            }
        }
    } else {
        esp_err_t stream_stop = camera_stream_set_local_mjpeg_enabled(false);
        operation = app_wifi_stop_softap();
        if (operation == ESP_OK && stream_stop != ESP_OK) {
            operation = stream_stop;
        }
    }
    const bool actual_enabled = app_wifi_softap_is_started() &&
                                camera_stream_local_mjpeg_is_enabled();
    s_ap_stream_requested = actual_enabled;
    if (operation == ESP_OK && actual_enabled != enabled) {
        operation = ESP_ERR_INVALID_STATE;
    }

    esp_err_t route_after = esp_netif_set_default_netif(s_rndis_netif);
    if (route_after != ESP_OK) {
        ESP_LOGE(TAG, "Cannot restore RNDIS route after AP transition: %s",
                 esp_err_to_name(route_after));
    }
    return operation != ESP_OK ? operation : route_after;
}
#endif

#if CONFIG_EXAMPLE_ENABLE_AT_CMD
typedef struct {
    usbh_cdc_port_handle_t cdc_port;      /*!< CDC port handle */
    at_handle_t at_handle;                /*!< AT command parser handle */
} at_ctx_t;

/* Keep the registration and RNDIS-control CDC ports independent. L610 mode 33
 * exposes both at once; reusing one context races with delayed close callbacks. */
static at_ctx_t s_l610_registration_ctx = {0};
at_ctx_t g_at_ctx = {0};
static volatile bool s_l610_at_configured = false;
static bool s_l610_network_registered = false;

static bool l610_rndis_connect_ready(void *ctx)
{
    return s_l610_at_configured;
}

static esp_err_t _at_send_cmd(const char *command, size_t length, void *usr_data)
{
    at_ctx_t *at_ctx = (at_ctx_t *)usr_data;
    return usbh_cdc_write_bytes(at_ctx->cdc_port, (const uint8_t *)command, length, pdMS_TO_TICKS(500));
}

static void _at_port_closed_cb(usbh_cdc_port_handle_t cdc_port_handle, void *arg)
{
    at_ctx_t *at_ctx = (at_ctx_t *)arg;
    ESP_LOGI(TAG, "AT port closed");
    at_ctx->cdc_port = NULL;
    if (at_ctx == &s_l610_registration_ctx) {
        s_l610_network_registered = false;
    } else if (at_ctx == &g_at_ctx) {
        s_l610_at_configured = false;
    }

    if (at_ctx->at_handle) {
        modem_at_stop(at_ctx->at_handle);
        modem_at_parser_destroy(at_ctx->at_handle);
        at_ctx->at_handle = NULL;
    }
}

static void _at_recv_data_cb(usbh_cdc_port_handle_t cdc_port_handle, void *arg)
{
    at_ctx_t *at_ctx = (at_ctx_t *)arg;
    size_t length = 0;
    usbh_cdc_get_rx_buffer_size(cdc_port_handle, &length);
    if (at_ctx->at_handle == NULL) {
        ESP_LOGW(TAG, "Discarding %zu bytes from a closed AT interface", length);
        return;
    }
    char *buffer;
    size_t buffer_remain;
    modem_at_get_response_buffer(at_ctx->at_handle, &buffer, &buffer_remain);
    if (buffer_remain < length) {
        length = buffer_remain;
        ESP_LOGE(TAG, "data size is too big, truncated to %zu", length);
    }
    usbh_cdc_read_bytes(cdc_port_handle, (uint8_t *)buffer, &length, 0);
    // Parse the AT command response
    modem_at_write_response_done(at_ctx->at_handle, length);
}

static esp_err_t at_init(at_ctx_t *at_ctx, uint8_t interface_num)
{
    ESP_LOGI(TAG, "Opening L610 AT interface %u", interface_num);
    // Open a CDC port for AT command
    usbh_cdc_port_handle_t _port = usb_rndis_get_cdc_port_handle(s_rndis_eth_driver);
    usb_device_handle_t _dev_hdl = NULL;
    ESP_ERROR_CHECK(usbh_cdc_get_dev_handle(_port, &_dev_hdl));
    usb_device_info_t device_info;
    ESP_ERROR_CHECK(usb_host_device_info(_dev_hdl, &device_info));
    usbh_cdc_port_config_t cdc_port_config = {
        .dev_addr = device_info.dev_addr,
        .itf_num = interface_num,
        .in_transfer_buffer_size = 512,
        .out_transfer_buffer_size = 512,
        .cbs = {
            .notif_cb = NULL,
            .recv_data = _at_recv_data_cb,
            .closed = _at_port_closed_cb,
            .user_data = at_ctx,
        },
    };
    ESP_RETURN_ON_ERROR(usbh_cdc_port_open(&cdc_port_config, &at_ctx->cdc_port), TAG, "Failed to open AT CDC port");

    // init the AT command parser
    modem_at_config_t at_config = {
        .send_buffer_length = 256,
        .recv_buffer_length = 256,
        .io = {
            .send_cmd = _at_send_cmd,
            .usr_data = at_ctx,
        }
    };
    at_ctx->at_handle = modem_at_parser_create(&at_config);
    if (at_ctx->at_handle == NULL) {
        usbh_cdc_port_close(at_ctx->cdc_port);
        at_ctx->cdc_port = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = modem_at_start(at_ctx->at_handle);
    if (ret != ESP_OK) {
        modem_at_parser_destroy(at_ctx->at_handle);
        at_ctx->at_handle = NULL;
        usbh_cdc_port_close(at_ctx->cdc_port);
        at_ctx->cdc_port = NULL;
    }
    return ret;
}

static esp_err_t l610_wait_until_ready(at_handle_t at_handle)
{
    for (int retry = 0; retry < 30; retry++) {
        if (at_cmd_at(at_handle) == ESP_OK) {
            break;
        }
        if (retry == 29) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    at_cmd_set_echo(at_handle, false);
    at_send_command_response_ok(at_handle, "AT+CMEE=2");

    for (int retry = 0; retry < 60; retry++) {
        esp_modem_pin_state_t pin_state = PIN_UNKNOWN;
        if (at_cmd_read_pin(at_handle, &pin_state) == ESP_OK && pin_state == PIN_READY) {
            ESP_LOGI(TAG, "L610 SIM is ready");
            break;
        }
        if (retry == 59) {
            ESP_LOGE(TAG, "L610 SIM did not become ready");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (int retry = 0; retry < 90; retry++) {
        esp_modem_at_cereg_t registration = {0};
        if (at_cmd_get_network_reg_status(at_handle, &registration) == ESP_OK &&
                (registration.stat == 1 || registration.stat == 5)) {
            ESP_LOGI(TAG, "L610 registered on LTE network (CEREG stat=%d)", registration.stat);
            return ESP_OK;
        }
        if ((retry % 10) == 0) {
            ESP_LOGI(TAG, "Waiting for L610 network registration...");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_ERR_TIMEOUT;
}

/* IF7 is the L610's working registration AT port on the ADP-L610 board.
 * IF2 is used separately below to request the RNDIS data session. */
static esp_err_t l610_prepare_network(at_handle_t at_handle)
{
    ESP_RETURN_ON_ERROR(l610_wait_until_ready(at_handle), TAG, "L610 did not become network-ready");
    ESP_RETURN_ON_ERROR(at_send_command_response_ok(at_handle, "AT+CGATT=1"), TAG, "L610 packet attach failed");

    esp_modem_at_pdp_t pdp = {
        .cid = 1,
        .type = "IP",
        .apn = CONFIG_L610_APN,
    };
    ESP_RETURN_ON_ERROR(at_cmd_set_pdp_context(at_handle, &pdp), TAG, "Failed to configure L610 APN");

    ESP_LOGI(TAG, "L610 network preparation completed on IF7 (APN=%s, CID=1)", CONFIG_L610_APN);
    return ESP_OK;
}

static esp_err_t l610_enable_rndis(at_handle_t at_handle, uint8_t interface_num)
{
    ESP_LOGI(TAG, "Requesting L610 RNDIS data session on IF%u", interface_num);

    for (int retry = 1; retry <= 3; retry++) {
        /* L610 firmware owns PDP activation for a RNDIS session. Do not issue
         * CGACT before GTRNDIS on the tested L610-CN firmware. */
        esp_err_t ret = at_send_command_response_ok(at_handle, "AT+GTRNDIS=1,1");
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "L610 RNDIS data session enabled automatically (APN=%s, CID=1)", CONFIG_L610_APN);
            return ESP_OK;
        }
        if (xEventGroupGetBits(s_event_group) & EVENT_GOT_IP_BIT) {
            ESP_LOGI(TAG, "L610 RNDIS already has an IP address");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "AT+GTRNDIS=1,1 attempt %d/3 failed", retry);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* On an ESP-only warm reset the L610 can keep its previous RNDIS session.
     * It then returns ERROR to a duplicate enable command even though the USB
     * RNDIS function is already enumerated. Let the host initialize and DHCP
     * prove whether that retained session is usable. */
    if (usb_rndis_get_cdc_port_handle(s_rndis_eth_driver) != NULL) {
        ESP_LOGW(TAG,
                 "L610 RNDIS command was not acknowledged, but USB RNDIS is present; "
                 "continuing for warm-reset recovery");
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void l610_at_manager_task(void *arg)
{
    const uint8_t registration_interface = CONFIG_EXAMPLE_AT_INTERFACE_NUM;
    const uint8_t rndis_control_interface = 2;

    while (true) {
        usbh_cdc_port_handle_t rndis_port = usb_rndis_get_cdc_port_handle(s_rndis_eth_driver);
        if (rndis_port == NULL) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!s_l610_network_registered) {
            if (s_l610_registration_ctx.at_handle == NULL &&
                    at_init(&s_l610_registration_ctx, registration_interface) != ESP_OK) {
                ESP_LOGW(TAG, "L610 registration interface %u unavailable", registration_interface);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            if (s_l610_registration_ctx.at_handle != NULL) {
                if (l610_prepare_network(s_l610_registration_ctx.at_handle) == ESP_OK) {
                    s_l610_network_registered = true;
                    ESP_LOGI(TAG, "Keeping IF%u for registration and opening IF%u for RNDIS control",
                             registration_interface, rndis_control_interface);
                    continue;
                }
                ESP_LOGE(TAG, "L610 network preparation failed on IF%u; retrying", registration_interface);
            }
        }

        if (!s_l610_at_configured && g_at_ctx.at_handle == NULL &&
                at_init(&g_at_ctx, rndis_control_interface) != ESP_OK) {
            ESP_LOGW(TAG, "L610 RNDIS control interface %u unavailable", rndis_control_interface);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (g_at_ctx.at_handle != NULL && !s_l610_at_configured) {
            if (l610_enable_rndis(g_at_ctx.at_handle, rndis_control_interface) == ESP_OK) {
                s_l610_at_configured = true;
                xEventGroupSetBits(s_event_group, EVENT_AT_READY_BIT);
            } else {
                ESP_LOGW(TAG, "L610 RNDIS setup failed on IF%u; falling back to IF%u",
                         rndis_control_interface, registration_interface);
                if (s_l610_registration_ctx.at_handle != NULL &&
                        l610_enable_rndis(s_l610_registration_ctx.at_handle, registration_interface) == ESP_OK) {
                    ESP_LOGI(TAG, "L610 RNDIS data session enabled via IF%u fallback", registration_interface);
                    s_l610_at_configured = true;
                    xEventGroupSetBits(s_event_group, EVENT_AT_READY_BIT);
                } else {
                    ESP_LOGE(TAG, "L610 RNDIS setup failed on IF%u and IF%u; retrying",
                             rndis_control_interface, registration_interface);
                    if (g_at_ctx.cdc_port != NULL) {
                        usbh_cdc_port_close(g_at_ctx.cdc_port);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    continue;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

static void iot_event_handle(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == IOT_ETH_EVENT) {
        switch (event_id) {
        case IOT_ETH_EVENT_START:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_START");
            break;
        case IOT_ETH_EVENT_STOP:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_STOP");
            break;
        case IOT_ETH_EVENT_CONNECTED:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_CONNECTED");
            break;
        case IOT_ETH_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_DISCONNECTED");
            xEventGroupClearBits(s_event_group, EVENT_GOT_IP_BIT);
            camera_stream_set_network_ready(false);
            telemetry_uart_set_network_ready(false);
            break;
        default:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_UNKNOWN");
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_dns_info_t dns = {0};
        char uplink_ifname[6] = {0};

        ESP_LOGI(TAG, "L610 RNDIS got IPv4: " IPSTR ", netmask: " IPSTR ", gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
        if (esp_netif_get_dns_info(s_rndis_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
                dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
            snprintf(s_rndis_dns, sizeof(s_rndis_dns), IPSTR,
                     IP2STR(&dns.ip.u_addr.ip4));
            ESP_LOGI(TAG, "L610 RNDIS DNS: %s", s_rndis_dns);
        } else {
            snprintf(s_rndis_dns, sizeof(s_rndis_dns), "223.5.5.5");
            ESP_LOGW(TAG, "No upstream DNS supplied; fallback is %s", s_rndis_dns);
        }
        ESP_ERROR_CHECK(esp_netif_set_default_netif(s_rndis_netif));
        if (esp_netif_get_netif_impl_name(s_rndis_netif, uplink_ifname) == ESP_OK) {
            camera_stream_set_uplink_interface(uplink_ifname);
            telemetry_uart_set_uplink_interface(uplink_ifname);
        } else {
            ESP_LOGW(TAG, "Could not resolve RNDIS lwIP interface name; using default route");
            camera_stream_set_uplink_interface(NULL);
            telemetry_uart_set_uplink_interface(NULL);
        }
        camera_stream_set_network_ready(true);
        xEventGroupSetBits(s_event_group, EVENT_GOT_IP_BIT);
    }
}

static void install_rndis(uint16_t idVendor, uint16_t idProduct, const char *netif_name)
{
    esp_err_t ret = ESP_OK;
    iot_eth_handle_t eth_handle = NULL;
    iot_eth_netif_glue_handle_t glue = NULL;

    usb_device_match_id_t *dev_match_id = calloc(2, sizeof(usb_device_match_id_t));
    if (dev_match_id == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for device match ID");
        return;
    }
    dev_match_id[0].match_flags = USB_DEVICE_ID_MATCH_VID_PID;
    dev_match_id[0].idVendor = idVendor;
    dev_match_id[0].idProduct = idProduct;
    memset(&dev_match_id[1], 0, sizeof(usb_device_match_id_t)); // end of list
    iot_usbh_rndis_config_t rndis_cfg = {
        .match_id_list = dev_match_id,
#if CONFIG_EXAMPLE_ENABLE_AT_CMD
        .connect_ready_cb = l610_rndis_connect_ready,
        .connect_ready_ctx = NULL,
#endif
    };

    ret = iot_eth_new_usb_rndis(&rndis_cfg, &s_rndis_eth_driver);
    if (ret != ESP_OK || s_rndis_eth_driver == NULL) {
        ESP_LOGE(TAG, "Failed to create USB RNDIS driver");
        free(dev_match_id);
        return;
    }
    // Note: dev_match_id is now managed by the driver, don't free it here

    iot_eth_config_t eth_cfg = {
        .driver = s_rndis_eth_driver,
        .stack_input = NULL,
    };
    ret = iot_eth_install(&eth_cfg, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB RNDIS driver");
        return;
    }

    esp_netif_inherent_config_t _inherent_eth_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    _inherent_eth_config.if_key = netif_name;
    _inherent_eth_config.if_desc = netif_name;
    esp_netif_config_t netif_cfg = {
        .base = &_inherent_eth_config,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_rndis_netif = esp_netif_new(&netif_cfg);
    if (s_rndis_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create network interface");
        return;
    }

    glue = iot_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        return;
    }
    esp_netif_attach(s_rndis_netif, glue);
    iot_eth_start(eth_handle);
}

static void start_camera_task(void)
{
    if (s_camera_task_started) {
        return;
    }
#if CONFIG_VIDEO_CAMERA_ENABLE
    if (camera_stream_start() != ESP_OK) {
        ESP_LOGE(TAG, "Camera UDP uploader could not be started");
    }
#else
    ESP_LOGW(TAG, "OV5640 capture is disabled in this diagnostic build; no XCLK or video task");
#endif
    s_camera_task_started = true;
}

static void start_telemetry_task(void)
{
    if (s_telemetry_task_started) {
        return;
    }
    if (telemetry_uart_start() != ESP_OK) {
        ESP_LOGE(TAG, "Unified USART telemetry could not be started");
    }
    s_telemetry_task_started = true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 + OV5640 + L610 RNDIS UDP video uplink");
    ESP_LOGI(TAG, "Expected USB device: %04X:%04X (L610 USB mode 33)", L610_USB_VID, L610_RNDIS_PID);
    ESP_LOGI(TAG, "Power path: ESP32-S3 Host VBUS -> USB-C cable -> ADP-L610; do not attach a second 5 V source");
#ifdef CONFIG_L610_SOFTAP_SHARE_ENABLE
    ESP_LOGI(TAG, "Wi-Fi SoftAP/NAPT runtime control enabled; default is on after L610 DHCP");
#else
    ESP_LOGI(TAG, "Wi-Fi SoftAP runtime control is unsupported in this profile");
#endif

    /* Initialize NVS for Wi-Fi storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated and needs to be erased
         * Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize default TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_event_group = xEventGroupCreate();
    ESP_RETURN_VOID_ON_FALSE(s_event_group != NULL, TAG, "Failed to create event group");
    esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, iot_event_handle, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, iot_event_handle, NULL);

#ifdef CONFIG_L610_SOFTAP_SHARE_ENABLE
    telemetry_uart_configure_ap_stream(true, TELEMETRY_AP_STREAM_STARTING,
                                       set_ap_stream_enabled, NULL);
#else
    telemetry_uart_configure_ap_stream(false, TELEMETRY_AP_STREAM_UNSUPPORTED,
                                       NULL, NULL);
#endif
    /* Telemetry owns the cloud control socket, so it must not depend on AP state. */
    start_telemetry_task();
#ifndef CONFIG_L610_SOFTAP_SHARE_ENABLE
    start_camera_task();
#endif

    // install usbh cdc driver
    usbh_cdc_driver_config_t config = {
        .task_stack_size = 1024 * 4,
        .task_priority = configMAX_PRIORITIES - 1,
        .task_coreid = 0,
        .skip_init_usb_host_driver = false,
    };
    ESP_ERROR_CHECK(usbh_cdc_driver_install(&config));

    install_rndis(L610_USB_VID, L610_RNDIS_PID, "USB RNDIS0");
#if CONFIG_EXAMPLE_ENABLE_AT_CMD
    BaseType_t task_ok = xTaskCreate(l610_at_manager_task, "l610_at_manager", 6144, NULL, 8, NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_FAIL);
#endif

    ESP_LOGI(TAG, "Waiting for L610 enumeration, LTE registration, RNDIS and DHCP");

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, EVENT_AT_READY_BIT | EVENT_GOT_IP_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
#if CONFIG_EXAMPLE_ENABLE_AT_CMD
        if (bits & EVENT_AT_READY_BIT) {
            at_cmd_set_echo(g_at_ctx.at_handle, true);

            esp_modem_at_csq_t result;
            esp_err_t err = at_cmd_get_signal_quality(g_at_ctx.at_handle, &result);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Signal quality rssi: %d", result.rssi);
            }

            // Print modem information
            char str[128] = {0};
            at_cmd_get_manufacturer_id(g_at_ctx.at_handle, str, sizeof(str));
            ESP_LOGI(TAG, "Modem manufacturer ID: %s", str);
            str[0] = '\0'; // clear the string buffer
            at_cmd_get_module_id(g_at_ctx.at_handle, str, sizeof(str));
            ESP_LOGI(TAG, "Modem module ID: %s", str);
            str[0] = '\0'; // clear the string buffer
            at_cmd_get_revision_id(g_at_ctx.at_handle, str, sizeof(str));
            ESP_LOGI(TAG, "Modem revision ID: %s", str);
            str[0] = '\0'; // clear the string buffer
            at_cmd_get_pdp_context(g_at_ctx.at_handle, str, sizeof(str));
            ESP_LOGI(TAG, "Modem PDP context: %s", str);
        }
#endif
        if (bits & EVENT_GOT_IP_BIT) {
#ifdef CONFIG_L610_SOFTAP_SHARE_ENABLE
            snprintf(s_modem_wifi_config.dns, sizeof(s_modem_wifi_config.dns),
                     "%s", s_rndis_dns);
            if (s_ap_stream_requested) {
                telemetry_uart_set_ap_stream_state(TELEMETRY_AP_STREAM_STARTING, ESP_OK);
                ESP_LOGI(TAG, "Starting default SoftAP/NAPT; free heap=%lu bytes",
                         (unsigned long)esp_get_free_heap_size());
                esp_err_t ap_ret = set_ap_stream_enabled(true, NULL);
                telemetry_uart_set_ap_stream_state(
                    ap_ret == ESP_OK ? TELEMETRY_AP_STREAM_ENABLED
                                     : TELEMETRY_AP_STREAM_ERROR,
                    ap_ret);
                if (ap_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Internet sharing ready: SSID=%s, gateway=%s, DNS=%s",
                             s_modem_wifi_config.ap_ssid, CONFIG_SERVER_IP,
                             s_modem_wifi_config.dns);
                } else {
                    ESP_LOGE(TAG, "Default SoftAP/NAPT start failed: %s",
                             esp_err_to_name(ap_ret));
                }
            }
            /* Preserve the verified 8 FPS profile ordering: AP first, camera second. */
            start_camera_task();
            /* The DHCP event arrived before camera_stream_start() created its
             * event group, so publish RNDIS readiness again after task startup. */
            camera_stream_set_network_ready(true);
#endif
            telemetry_uart_set_network_ready(true);
            ESP_LOGI(TAG, "L610 RNDIS video uplink enabled, DNS=%s", s_rndis_dns);
#if CONFIG_EXAMPLE_USB_RNDIS_DOWNLOAD_SPEED_TEST
            ESP_LOGW(TAG, "Download speed test is unavailable in the camera-only build");
#endif
        }
    }
}
