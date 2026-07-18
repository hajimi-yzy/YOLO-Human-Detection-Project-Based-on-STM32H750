/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "lwip/ip_addr.h"
#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"
#include "app_wifi.h"

#define EXAMPLE_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY
#define EXAMPLE_IP_ADDR CONFIG_SERVER_IP
#define WIFI_MANAGER_PORT 8081
#define WIFI_MANAGER_MAX_BODY 256
#define WIFI_MANAGER_MAX_SCAN_RESULTS 20
#define SOFTAP_STARTED_BIT BIT0
#define SOFTAP_STOPPED_BIT BIT1
#define SOFTAP_TRANSITION_TIMEOUT_MS 5000

static const char *TAG = "app wifi";

static esp_netif_t *wifi_ap_netif;
static esp_netif_t *wifi_sta_netif;
static SemaphoreHandle_t s_wifi_lock;
static EventGroupHandle_t s_wifi_events;
static httpd_handle_t s_wifi_httpd;
static bool s_wifi_initialized;
static bool s_wifi_driver_started;
static bool s_softap_started;
static bool s_sta_feature_enabled;
static bool s_sta_connect_requested;
static bool s_sta_connected;
static bool s_scan_in_progress;
static bool s_wifi_uplink_selected;
static bool s_control_busy;
static unsigned s_ignore_disconnect_events;
static int s_retry_num;
static char s_sta_ssid[33];
static char s_sta_ip[16];
static char s_last_error[96];
static app_wifi_uplink_callback_t s_uplink_callback;
static void *s_uplink_callback_ctx;

static app_wifi_active_uplink_t s_active_uplink = APP_WIFI_UPLINK_NONE;

static esp_err_t start_wifi_manager_http(void);

static void set_last_error(const char *error)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", error == NULL ? "" : error);
}

static esp_err_t notify_uplink_change(void)
{
    if (s_uplink_callback == NULL) {
        s_active_uplink = APP_WIFI_UPLINK_NONE;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = s_uplink_callback(s_wifi_uplink_selected, s_sta_connected,
                                      wifi_sta_netif, s_uplink_callback_ctx);
    if (ret == ESP_OK) {
        s_active_uplink = s_wifi_uplink_selected && s_sta_connected
            ? APP_WIFI_UPLINK_WIFI : APP_WIFI_UPLINK_L610;
    } else {
        s_active_uplink = APP_WIFI_UPLINK_NONE;
    }
    return ret;
}

static wifi_mode_t desired_wifi_mode(bool ap_enabled)
{
    /* The local management request itself arrives over SoftAP.  Create the
     * dormant STA control block with the AP so enabling STA later never tears
     * down the interface carrying the HTTP response.  feature_enabled still
     * gates every scan/connect/reconnect operation. */
    if (ap_enabled) {
        return WIFI_MODE_APSTA;
    }
    if (s_sta_feature_enabled) {
        return WIFI_MODE_STA;
    }
    return WIFI_MODE_NULL;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_softap_started = true;
        if (s_wifi_events != NULL) {
            xEventGroupClearBits(s_wifi_events, SOFTAP_STOPPED_BIT);
            xEventGroupSetBits(s_wifi_events, SOFTAP_STARTED_BIT);
        }
        ESP_LOGI(TAG, "SoftAP start event received");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_softap_started = false;
        if (s_wifi_events != NULL) {
            xEventGroupClearBits(s_wifi_events, SOFTAP_STARTED_BIT);
            xEventGroupSetBits(s_wifi_events, SOFTAP_STOPPED_BIT);
        }
        ESP_LOGI(TAG, "SoftAP stop event received");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_feature_enabled && s_sta_connect_requested && !s_control_busy) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        if (s_ignore_disconnect_events > 0) {
            --s_ignore_disconnect_events;
        } else if (s_sta_feature_enabled && s_sta_connect_requested && !s_control_busy &&
                s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            ++s_retry_num;
            ESP_LOGW(TAG, "Wi-Fi STA disconnected (reason=%u), retry %d/%d",
                     event == NULL ? 0 : event->reason, s_retry_num,
                     EXAMPLE_ESP_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else if (s_sta_feature_enabled && s_sta_connect_requested) {
            snprintf(s_last_error, sizeof(s_last_error),
                     "connection failed (reason %u)", event == NULL ? 0 : event->reason);
            ESP_LOGW(TAG, "Wi-Fi STA retry limit reached");
        }
        if (s_wifi_uplink_selected) {
            esp_err_t ret = notify_uplink_change();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "No fallback cloud uplink after Wi-Fi disconnect: %s",
                         esp_err_to_name(ret));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (!s_sta_feature_enabled || !s_sta_connect_requested) {
            ESP_LOGW(TAG, "Ignoring stale Wi-Fi GOT_IP event while STA is disabled");
            return;
        }
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
                strncmp((const char *)ap_info.ssid, s_sta_ssid,
                        sizeof(ap_info.ssid)) != 0) {
            ESP_LOGW(TAG, "Ignoring stale Wi-Fi GOT_IP event for another SSID");
            return;
        }
        ip_event_got_ip_t *event = event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_sta_connected = true;
        set_last_error(NULL);
        ESP_LOGI(TAG, "Wi-Fi STA connected: SSID=%s IP=%s", s_sta_ssid, s_sta_ip);
        if (s_wifi_uplink_selected) {
            esp_err_t ret = notify_uplink_change();
            if (ret != ESP_OK) {
                snprintf(s_last_error, sizeof(s_last_error),
                         "cloud uplink switch failed: %s", esp_err_to_name(ret));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        if (s_wifi_uplink_selected) {
            esp_err_t ret = notify_uplink_change();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "No fallback cloud uplink after Wi-Fi lost IP: %s",
                         esp_err_to_name(ret));
            }
        }
    }
}

static esp_err_t set_dhcps_running(esp_netif_t *wifi_netif, bool running)
{
    esp_err_t ret = running ? esp_netif_dhcps_start(wifi_netif)
                            : esp_netif_dhcps_stop(wifi_netif);
    if ((!running && ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) ||
            (running && ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)) {
        return ESP_OK;
    }
    return ret;
}

static esp_err_t wait_for_softap_event(EventBits_t bit, const char *operation)
{
    if (s_wifi_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t received = xEventGroupWaitBits(
        s_wifi_events, bit, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(SOFTAP_TRANSITION_TIMEOUT_MS));
    if ((received & bit) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for SoftAP %s event", operation);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t wait_for_softap_netif_up(void)
{
    for (int retry = 0; retry < 50; ++retry) {
        if (wifi_ap_netif != NULL && esp_netif_is_netif_up(wifi_ap_netif)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "SoftAP start event completed but netif is still down");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t wifi_init_softap(esp_netif_t *wifi_netif,
                                  const modem_wifi_config_t *config)
{
    const size_t ssid_len = strnlen(config->ap_ssid, sizeof(config->ap_ssid));
    const size_t password_len = strnlen(config->ap_password,
                                        sizeof(config->ap_password));
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= 32,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid SoftAP SSID length");
    ESP_RETURN_ON_FALSE(password_len == 0 ||
                        (password_len >= 8 && password_len <= 63),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid SoftAP password length");

    if (strcmp(EXAMPLE_IP_ADDR, "192.168.4.1")) {
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr = ipaddr_addr(EXAMPLE_IP_ADDR);
        ip.gw.addr = ipaddr_addr(EXAMPLE_IP_ADDR);
        ip.netmask.addr = ipaddr_addr("255.255.255.0");
        ESP_RETURN_ON_ERROR(set_dhcps_running(wifi_netif, false), TAG,
                            "Failed to stop SoftAP DHCP server");
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(wifi_netif, &ip), TAG,
                            "Failed to configure SoftAP address");
    }

    esp_netif_dns_info_t dns = {0};
    const char *dns_addr = config->dns[0] ? config->dns : "223.5.5.5";
    dns.ip.u_addr.ip4.addr = ipaddr_addr(dns_addr);
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    ESP_RETURN_ON_ERROR(set_dhcps_running(wifi_netif, false), TAG,
                        "Failed to stop SoftAP DHCP server");
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(wifi_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value,
                               sizeof(dhcps_dns_value)),
        TAG, "Failed to enable DHCP DNS offer");
    ESP_RETURN_ON_ERROR(
        esp_netif_set_dns_info(wifi_netif, ESP_NETIF_DNS_MAIN, &dns), TAG,
        "Failed to configure SoftAP DNS");

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.ap.ssid, config->ap_ssid, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;
    memcpy(wifi_config.ap.password, config->ap_password, password_len);
    wifi_config.ap.max_connection = config->max_connection;
    wifi_config.ap.authmode = password_len == 0
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.channel = config->channel;
    wifi_config.ap.ssid_hidden = config->ssid_hidden;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG,
                        "Failed to configure SoftAP");
    ESP_LOGI(TAG, "SoftAP configured: SSID=%s, DNS=%s", config->ap_ssid,
             dns_addr);
    return ESP_OK;
}

static esp_err_t initialize_wifi_once(void)
{
    if (s_wifi_initialized) {
        return start_wifi_manager_http();
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS initialization failed");

    wifi_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(wifi_ap_netif != NULL && wifi_sta_netif != NULL,
                        ESP_ERR_NO_MEM, TAG, "Failed to create Wi-Fi netifs");

    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create Wi-Fi lifecycle events");
    xEventGroupSetBits(s_wifi_events, SOFTAP_STOPPED_BIT);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   event_handler, NULL),
        TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   event_handler, NULL),
        TAG, "IP event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                   event_handler, NULL),
        TAG, "IP lost event registration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Wi-Fi RAM storage configuration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_NULL), TAG,
                        "Wi-Fi initial mode configuration failed");

    s_wifi_initialized = true;
    return start_wifi_manager_http();
}

static esp_err_t start_driver_locked(wifi_mode_t mode)
{
    ESP_RETURN_ON_FALSE(mode != WIFI_MODE_NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Cannot start Wi-Fi in NULL mode");
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&current_mode), TAG,
                        "Failed to read Wi-Fi mode");
    if (current_mode != mode) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG,
                            "Failed to set Wi-Fi mode");
    }
    if (!s_wifi_driver_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG,
                            "Failed to start Wi-Fi driver");
        s_wifi_driver_started = true;
    }
    return ESP_OK;
}

static esp_err_t stop_softap_locked(void)
{
    esp_err_t first_error = ESP_OK;
    if (wifi_ap_netif != NULL) {
        esp_err_t ret = esp_netif_napt_disable(wifi_ap_netif);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to disable SoftAP NAPT: %s",
                     esp_err_to_name(ret));
            first_error = ret;
        }
        ret = set_dhcps_running(wifi_ap_netif, false);
        if (ret != ESP_OK && first_error == ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop SoftAP DHCP server: %s",
                     esp_err_to_name(ret));
            first_error = ret;
        }
    }

    const bool was_up = wifi_ap_netif != NULL &&
                        esp_netif_is_netif_up(wifi_ap_netif);
    bool stop_confirmed = !was_up;
    if (s_wifi_driver_started) {
        xEventGroupClearBits(s_wifi_events, SOFTAP_STOPPED_BIT);
        wifi_mode_t next_mode = desired_wifi_mode(false);
        esp_err_t ret;
        if (next_mode == WIFI_MODE_NULL) {
            ret = esp_wifi_stop();
            if (ret == ESP_OK) {
                s_wifi_driver_started = false;
                esp_wifi_set_mode(WIFI_MODE_NULL);
                stop_confirmed = true;
            }
        } else {
            ret = esp_wifi_set_mode(next_mode);
            if (ret == ESP_OK) {
                ret = wait_for_softap_event(SOFTAP_STOPPED_BIT, "stop");
                stop_confirmed = ret == ESP_OK;
            }
        }
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        } else if (ret == ESP_OK && was_up && next_mode == WIFI_MODE_NULL) {
            ret = wait_for_softap_event(SOFTAP_STOPPED_BIT, "stop");
            stop_confirmed = ret == ESP_OK;
            if (ret != ESP_OK && first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }
    if (stop_confirmed) {
        s_softap_started = false;
    }
    return first_error;
}

static esp_err_t ensure_wifi_lock(void)
{
    if (s_wifi_lock == NULL) {
        s_wifi_lock = xSemaphoreCreateMutex();
    }
    return s_wifi_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t app_wifi_start_softap(const modem_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && (config->mode & WIFI_MODE_AP),
                        ESP_ERR_INVALID_ARG, TAG, "SoftAP configuration is required");
    ESP_RETURN_ON_ERROR(ensure_wifi_lock(), TAG, "Failed to create Wi-Fi lock");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Failed to lock Wi-Fi control");

    esp_err_t ret = ESP_OK;
    s_control_busy = true;
    if (s_softap_started) {
        goto done;
    }
    ret = initialize_wifi_once();
    if (ret != ESP_OK) {
        goto done;
    }

    xEventGroupClearBits(s_wifi_events, SOFTAP_STARTED_BIT);
    const wifi_mode_t mode = desired_wifi_mode(true);
    if (!s_wifi_driver_started) {
        ret = esp_wifi_set_mode(mode);
        if (ret == ESP_OK) {
            ret = wifi_init_softap(wifi_ap_netif, config);
        }
        if (ret == ESP_OK) {
            ret = esp_wifi_start();
            if (ret == ESP_OK) {
                s_wifi_driver_started = true;
            }
        }
    } else {
        ret = esp_wifi_set_mode(mode);
        if (ret == ESP_OK) {
            ret = wifi_init_softap(wifi_ap_netif, config);
        }
    }
    if (ret != ESP_OK) {
        goto rollback;
    }
    if (!esp_netif_is_netif_up(wifi_ap_netif)) {
        ret = wait_for_softap_event(SOFTAP_STARTED_BIT, "start");
        if (ret != ESP_OK) {
            goto rollback;
        }
    }
    ret = wait_for_softap_netif_up();
    if (ret != ESP_OK) {
        goto rollback;
    }
    ret = set_dhcps_running(wifi_ap_netif, true);
    if (ret != ESP_OK) {
        goto rollback;
    }
    ret = esp_wifi_set_bandwidth(WIFI_IF_AP, config->bandwidth);
    if (ret != ESP_OK) {
        goto rollback;
    }
    esp_netif_ip_info_t ip = {0};
    ret = esp_netif_get_ip_info(wifi_ap_netif, &ip);
    if (ret != ESP_OK) {
        goto rollback;
    }
    ret = esp_netif_napt_enable(wifi_ap_netif);
    if (ret != ESP_OK) {
        goto rollback;
    }
    s_softap_started = true;
    ESP_LOGI(TAG, "NAPT enabled on SoftAP " IPSTR, IP2STR(&ip.ip));
    goto done;

rollback:
    {
        esp_err_t operation_error = ret;
        stop_softap_locked();
        ret = operation_error;
    }
done:
    s_control_busy = false;
    xSemaphoreGive(s_wifi_lock);
    return ret;
}

esp_err_t app_wifi_stop_softap(void)
{
    if (s_wifi_lock == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Failed to lock Wi-Fi control");
    s_control_busy = true;
    esp_err_t ret = (s_softap_started ||
                     (wifi_ap_netif != NULL && esp_netif_is_netif_up(wifi_ap_netif)))
        ? stop_softap_locked() : ESP_OK;
    if (!s_softap_started) {
        ESP_LOGI(TAG, "SoftAP/NAPT stopped");
    }
    s_control_busy = false;
    xSemaphoreGive(s_wifi_lock);
    return ret;
}

bool app_wifi_softap_is_started(void)
{
    return s_softap_started;
}

void app_wifi_set_uplink_callback(app_wifi_uplink_callback_t callback,
                                  void *ctx)
{
    s_uplink_callback = callback;
    s_uplink_callback_ctx = ctx;
}

esp_err_t app_wifi_refresh_uplink(void)
{
    return notify_uplink_change();
}

bool app_wifi_sta_uplink_is_active(void)
{
    return s_wifi_uplink_selected && s_sta_connected &&
           s_active_uplink == APP_WIFI_UPLINK_WIFI;
}

esp_netif_t *app_wifi_get_sta_netif(void)
{
    return wifi_sta_netif;
}

esp_netif_t *app_wifi_get_ap_netif(void)
{
    return wifi_ap_netif;
}

esp_err_t app_wifi_get_status(app_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Wi-Fi status output is required");
    ESP_RETURN_ON_ERROR(ensure_wifi_lock(), TAG, "Failed to create Wi-Fi lock");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Wi-Fi control is busy");

    memset(status, 0, sizeof(*status));
    status->feature_enabled = s_sta_feature_enabled;
    status->scanning = s_scan_in_progress;
    status->connected = s_sta_connected;
    snprintf(status->ssid, sizeof(status->ssid), "%s", s_sta_ssid);
    snprintf(status->ip, sizeof(status->ip), "%s", s_sta_ip);
    status->wifi_uplink_selected = s_wifi_uplink_selected;
    status->active_uplink = s_active_uplink;
    snprintf(status->error, sizeof(status->error), "%s", s_last_error);

    wifi_ap_record_t ap_info = {0};
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        status->rssi_valid = true;
        status->rssi = ap_info.rssi;
    }
    xSemaphoreGive(s_wifi_lock);
    return ESP_OK;
}

static bool request_from_softap(httpd_req_t *req)
{
    if (!s_softap_started || wifi_ap_netif == NULL) {
        return false;
    }
    struct sockaddr_storage peer = {0};
    struct sockaddr_storage local = {0};
    socklen_t peer_len = sizeof(peer);
    socklen_t local_len = sizeof(local);
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0 ||
            getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
            peer.ss_family != AF_INET || local.ss_family != AF_INET) {
        return false;
    }
    esp_netif_ip_info_t ap_ip = {0};
    if (esp_netif_get_ip_info(wifi_ap_netif, &ap_ip) != ESP_OK) {
        return false;
    }
    const struct sockaddr_in *peer_v4 = (const struct sockaddr_in *)&peer;
    const struct sockaddr_in *local_v4 = (const struct sockaddr_in *)&local;
    return (peer_v4->sin_addr.s_addr & ap_ip.netmask.addr) ==
           (ap_ip.ip.addr & ap_ip.netmask.addr) &&
           local_v4->sin_addr.s_addr == ap_ip.ip.addr;
}

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_json(httpd_req_t *req, const char *status,
                           cJSON *root)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", atoi(status));
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, status, root);
}

static bool authorize_request(httpd_req_t *req)
{
    if (request_from_softap(req)) {
        return true;
    }
    send_error(req, "403 Forbidden", "Wi-Fi management is available only through the ESP hotspot");
    return false;
}

static cJSON *receive_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= WIFI_MANAGER_MAX_BODY) {
        return NULL;
    }
    char body[WIFI_MANAGER_MAX_BODY] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received,
                                 req->content_len - received);
        if (ret <= 0) {
            return NULL;
        }
        received += ret;
    }
    body[received] = '\0';
    return cJSON_Parse(body);
}

static cJSON *create_status_response(void)
{
    app_wifi_status_t status = {0};
    const esp_err_t status_ret = app_wifi_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddNumberToObject(root, "code", 200);
    cJSON_AddBoolToObject(data, "feature_enabled", status.feature_enabled);
    cJSON_AddBoolToObject(data, "scanning", status.scanning);
    cJSON_AddBoolToObject(data, "connected", status.connected);
    cJSON_AddStringToObject(data, "ssid", status.ssid);
    cJSON_AddStringToObject(data, "ip", status.ip);
    cJSON_AddBoolToObject(data, "wifi_uplink_selected", status.wifi_uplink_selected);
    const char *active_uplink = status.active_uplink == APP_WIFI_UPLINK_WIFI ? "wifi"
        : (status.active_uplink == APP_WIFI_UPLINK_L610 ? "l610" : "none");
    cJSON_AddStringToObject(data, "active_uplink", active_uplink);
    cJSON_AddBoolToObject(data, "uplink_applied",
                          status.active_uplink != APP_WIFI_UPLINK_NONE);
    cJSON_AddStringToObject(data, "error",
                           status_ret == ESP_OK ? status.error
                                                : esp_err_to_name(status_ret));
    if (status.rssi_valid) {
        cJSON_AddNumberToObject(data, "rssi", status.rssi);
    } else {
        cJSON_AddNullToObject(data, "rssi");
    }
    return root;
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    if (!authorize_request(req)) {
        return ESP_OK;
    }
    return send_json(req, "200 OK", create_status_response());
}

esp_err_t app_wifi_set_sta_feature_enabled(bool enabled)
{
    ESP_RETURN_ON_ERROR(ensure_wifi_lock(), TAG, "Failed to create Wi-Fi lock");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Failed to lock Wi-Fi control");
    esp_err_t ret = initialize_wifi_once();
    if (ret != ESP_OK) {
        goto done;
    }
    if (enabled == s_sta_feature_enabled) {
        goto done;
    }

    s_control_busy = true;
    if (enabled) {
        s_sta_feature_enabled = true;
        s_retry_num = 0;
        set_last_error(NULL);
        ret = start_driver_locked(desired_wifi_mode(s_softap_started));
        if (ret == ESP_OK) {
            esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            if (ps_ret != ESP_OK) {
                ESP_LOGW(TAG, "Could not enable Wi-Fi modem sleep: %s",
                         esp_err_to_name(ps_ret));
            }
            ESP_LOGI(TAG, "Wi-Fi STA feature enabled; scan remains idle until requested");
        } else {
            s_sta_feature_enabled = false;
        }
    } else {
        const bool previous_connect_requested = s_sta_connect_requested;
        const bool previous_uplink_selected = s_wifi_uplink_selected;
        const bool was_connected = s_sta_connected;
        s_sta_connect_requested = false;
        if (s_scan_in_progress) {
            esp_wifi_scan_stop();
        }
        if (was_connected) {
            ++s_ignore_disconnect_events;
        }
        esp_err_t disconnect_ret = esp_wifi_disconnect();
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT &&
                disconnect_ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "Wi-Fi disconnect during disable returned %s",
                     esp_err_to_name(disconnect_ret));
        }
        if (s_softap_started) {
            /* Keep the dormant STA control block while SoftAP is running.
             * Scanning/reconnection remain disabled by the feature flag. */
            ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        } else {
            ret = s_wifi_driver_started ? esp_wifi_stop() : ESP_OK;
            if (ret == ESP_OK) {
                s_wifi_driver_started = false;
                esp_wifi_set_mode(WIFI_MODE_NULL);
            }
        }
        if (ret != ESP_OK) {
            s_sta_connect_requested = previous_connect_requested;
            s_wifi_uplink_selected = previous_uplink_selected;
            if (previous_connect_requested) {
                esp_wifi_connect();
            }
            goto done;
        }

        s_sta_feature_enabled = false;
        s_wifi_uplink_selected = false;
        s_retry_num = 0;
        s_scan_in_progress = false;
        s_sta_connected = false;
        s_sta_ssid[0] = '\0';
        s_sta_ip[0] = '\0';
        esp_err_t uplink_ret = notify_uplink_change();
        if (uplink_ret == ESP_OK) {
            set_last_error(NULL);
        } else {
            snprintf(s_last_error, sizeof(s_last_error),
                     "no cloud uplink: %s", esp_err_to_name(uplink_ret));
        }
        ESP_LOGI(TAG, "Wi-Fi STA feature disabled; scanning and reconnect are stopped");
    }
done:
    s_control_busy = false;
    xSemaphoreGive(s_wifi_lock);
    return ret;
}

static esp_err_t wifi_enable_handler(httpd_req_t *req)
{
    if (!authorize_request(req)) {
        return ESP_OK;
    }
    cJSON *body = receive_json(req);
    cJSON *enabled = body == NULL ? NULL : cJSON_GetObjectItem(body, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "enabled must be a boolean");
    }
    esp_err_t ret = app_wifi_set_sta_feature_enabled(cJSON_IsTrue(enabled));
    cJSON_Delete(body);
    if (ret != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(ret));
    }
    return send_json(req, "200 OK", create_status_response());
}

static const char *auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default: return "secured";
    }
}

static bool auth_mode_supported(wifi_auth_mode_t authmode)
{
    return authmode == WIFI_AUTH_OPEN || authmode == WIFI_AUTH_WPA_PSK ||
           authmode == WIFI_AUTH_WPA2_PSK ||
           authmode == WIFI_AUTH_WPA_WPA2_PSK ||
           authmode == WIFI_AUTH_WPA3_PSK ||
           authmode == WIFI_AUTH_WPA2_WPA3_PSK;
}

static bool auth_mode_from_name(const char *name, wifi_auth_mode_t *authmode)
{
    if (name == NULL || authmode == NULL) {
        return false;
    }
    if (strcmp(name, "open") == 0) *authmode = WIFI_AUTH_OPEN;
    else if (strcmp(name, "wpa") == 0) *authmode = WIFI_AUTH_WPA_PSK;
    else if (strcmp(name, "wpa2") == 0) *authmode = WIFI_AUTH_WPA2_PSK;
    else if (strcmp(name, "wpa/wpa2") == 0) *authmode = WIFI_AUTH_WPA_WPA2_PSK;
    else if (strcmp(name, "wpa3") == 0) *authmode = WIFI_AUTH_WPA3_PSK;
    else if (strcmp(name, "wpa2/wpa3") == 0) *authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    else return false;
    return true;
}

esp_err_t app_wifi_scan_networks(app_wifi_network_t *networks,
                                 size_t capacity, size_t *count)
{
    ESP_RETURN_ON_FALSE(networks != NULL && count != NULL && capacity > 0,
                        ESP_ERR_INVALID_ARG, TAG, "Wi-Fi scan output is required");
    ESP_RETURN_ON_ERROR(ensure_wifi_lock(), TAG, "Failed to create Wi-Fi lock");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Wi-Fi control is busy");
    esp_err_t ret = ESP_OK;
    *count = 0;
    if (!s_sta_feature_enabled || !s_wifi_driver_started) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (s_scan_in_progress) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    s_scan_in_progress = true;
    s_control_busy = true;
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 120,
    };
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        goto done;
    }

    uint16_t record_count = (uint16_t)(capacity < APP_WIFI_MAX_SCAN_RESULTS
        ? capacity : APP_WIFI_MAX_SCAN_RESULTS);
    wifi_ap_record_t records[APP_WIFI_MAX_SCAN_RESULTS] = {0};
    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        goto done;
    }
    for (uint16_t index = 0; index < record_count; ++index) {
        app_wifi_network_t *network = &networks[index];
        memset(network, 0, sizeof(*network));
        snprintf(network->ssid, sizeof(network->ssid), "%.*s",
                 (int)sizeof(records[index].ssid), (char *)records[index].ssid);
        network->rssi = records[index].rssi;
        network->channel = records[index].primary;
        snprintf(network->security, sizeof(network->security), "%s",
                 auth_mode_name(records[index].authmode));
        network->secured = records[index].authmode != WIFI_AUTH_OPEN;
        network->supported = auth_mode_supported(records[index].authmode);
    }
    *count = record_count;

done:
    s_scan_in_progress = false;
    s_control_busy = false;
    set_last_error(ret == ESP_OK ? NULL : esp_err_to_name(ret));
    xSemaphoreGive(s_wifi_lock);
    return ret;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!authorize_request(req)) {
        return ESP_OK;
    }
    app_wifi_network_t records[WIFI_MANAGER_MAX_SCAN_RESULTS] = {0};
    size_t count = 0;
    esp_err_t ret = app_wifi_scan_networks(
        records, WIFI_MANAGER_MAX_SCAN_RESULTS, &count);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT) {
            return send_error(req, "409 Conflict",
                              ret == ESP_ERR_INVALID_STATE
                                  ? "Enable Wi-Fi connection before scanning"
                                  : "Wi-Fi control is busy");
        }
        return send_error(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", 200);
    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON *networks = cJSON_AddArrayToObject(data, "networks");
    for (size_t index = 0; index < count; ++index) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", records[index].ssid);
        cJSON_AddNumberToObject(network, "rssi", records[index].rssi);
        cJSON_AddNumberToObject(network, "channel", records[index].channel);
        cJSON_AddStringToObject(network, "security", records[index].security);
        cJSON_AddBoolToObject(network, "secured", records[index].secured);
        cJSON_AddBoolToObject(network, "supported", records[index].supported);
        cJSON_AddItemToArray(networks, network);
    }
    return send_json(req, "200 OK", root);
}

esp_err_t app_wifi_connect_sta(const char *ssid, const char *password,
                               const char *security)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && password != NULL && security != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "SSID, password and security are required");
    wifi_auth_mode_t authmode;
    ESP_RETURN_ON_FALSE(auth_mode_from_name(security, &authmode),
                        ESP_ERR_INVALID_ARG, TAG,
                        "Unsupported Wi-Fi security mode");
    ESP_RETURN_ON_ERROR(ensure_wifi_lock(), TAG, "Failed to create Wi-Fi lock");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Wi-Fi control is busy");
    esp_err_t ret = ESP_OK;
    if (!s_sta_feature_enabled || !s_wifi_driver_started) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 63 ||
            !auth_mode_supported(authmode) ||
            (authmode == WIFI_AUTH_OPEN && password_len != 0) ||
            (authmode != WIFI_AUTH_OPEN && password_len < 8)) {
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_len);
    memcpy(config.sta.password, password, password_len);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = authmode == WIFI_AUTH_WPA_WPA2_PSK
        ? WIFI_AUTH_WPA_PSK
        : (authmode == WIFI_AUTH_WPA2_WPA3_PSK ? WIFI_AUTH_WPA2_PSK : authmode);
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    s_control_busy = true;
    const bool was_connected = s_sta_connected;
    s_sta_connect_requested = false;
    if (was_connected) {
        ++s_ignore_disconnect_events;
    }
    esp_wifi_disconnect();
    ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (ret != ESP_OK) {
        goto done;
    }
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    s_sta_ip[0] = '\0';
    s_sta_connected = false;
    s_retry_num = 0;
    set_last_error(NULL);
    s_sta_connect_requested = true;
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        s_sta_connect_requested = false;
        set_last_error(esp_err_to_name(ret));
    }
done:
    s_control_busy = false;
    xSemaphoreGive(s_wifi_lock);
    return ret;
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    if (!authorize_request(req)) {
        return ESP_OK;
    }
    if (!s_sta_feature_enabled) {
        return send_error(req, "409 Conflict", "Enable Wi-Fi connection before connecting");
    }
    cJSON *body = receive_json(req);
    cJSON *ssid = body == NULL ? NULL : cJSON_GetObjectItem(body, "ssid");
    cJSON *password = body == NULL ? NULL : cJSON_GetObjectItem(body, "password");
    cJSON *security = body == NULL ? NULL : cJSON_GetObjectItem(body, "security");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
            !cJSON_IsString(security)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "ssid, password and security must be strings");
    }
    esp_err_t ret = app_wifi_connect_sta(ssid->valuestring, password->valuestring,
                                         security->valuestring);
    cJSON_Delete(body);
    if (ret == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request",
                          "Invalid SSID, password or security mode");
    }
    if (ret != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(ret));
    }
    cJSON *root = create_status_response();
    cJSON_ReplaceItemInObject(root, "code", cJSON_CreateNumber(202));
    return send_json(req, "202 Accepted", root);
}

esp_err_t app_wifi_select_cloud_uplink(bool use_wifi)
{
    ESP_RETURN_ON_ERROR(ensure_wifi_lock(), TAG, "Failed to create Wi-Fi lock");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Wi-Fi control is busy");
    if (use_wifi && (!s_sta_feature_enabled || !s_sta_connected)) {
        xSemaphoreGive(s_wifi_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const bool previous_selection = s_wifi_uplink_selected;
    s_wifi_uplink_selected = use_wifi;
    s_control_busy = true;
    esp_err_t ret = notify_uplink_change();
    if (ret != ESP_OK) {
        s_wifi_uplink_selected = previous_selection;
        esp_err_t restore_ret = notify_uplink_change();
        ESP_LOGE(TAG, "Cloud uplink switch to %s failed: %s; restore=%s",
                 use_wifi ? "Wi-Fi STA" : "L610 RNDIS", esp_err_to_name(ret),
                 esp_err_to_name(restore_ret));
        snprintf(s_last_error, sizeof(s_last_error),
                 "uplink switch failed: %s", esp_err_to_name(ret));
    } else {
        set_last_error(NULL);
        ESP_LOGI(TAG, "Cloud uplink selected: %s",
                 use_wifi ? "Wi-Fi STA" : "L610 RNDIS");
    }
    s_control_busy = false;
    xSemaphoreGive(s_wifi_lock);
    return ret;
}

static esp_err_t wifi_uplink_handler(httpd_req_t *req)
{
    if (!authorize_request(req)) {
        return ESP_OK;
    }
    cJSON *body = receive_json(req);
    cJSON *use_wifi = body == NULL ? NULL : cJSON_GetObjectItem(body, "use_wifi");
    if (!cJSON_IsBool(use_wifi)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "use_wifi must be a boolean");
    }
    bool selected = cJSON_IsTrue(use_wifi);
    cJSON_Delete(body);
    esp_err_t ret = app_wifi_select_cloud_uplink(selected);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT) {
            return send_error(req, "409 Conflict",
                              ret == ESP_ERR_INVALID_STATE
                                  ? "Connect Wi-Fi before selecting it as uplink"
                                  : "Wi-Fi control is busy");
        }
        return send_error(req, "503 Service Unavailable", s_last_error);
    }
    return send_json(req, "200 OK", create_status_response());
}

static esp_err_t wifi_options_handler(httpd_req_t *req)
{
    if (!authorize_request(req)) {
        return ESP_OK;
    }
    set_cors_headers(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t start_wifi_manager_http(void)
{
    if (s_wifi_httpd != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WIFI_MANAGER_PORT;
    config.ctrl_port = 32769;
    config.stack_size = 6144;
    config.max_uri_handlers = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_wifi_httpd, &config), TAG,
                        "Failed to start Wi-Fi manager HTTP server");

    const httpd_uri_t handlers[] = {
        { .uri = "/api/wifi/status", .method = HTTP_GET,
          .handler = wifi_status_handler },
        { .uri = "/api/wifi/enable", .method = HTTP_POST,
          .handler = wifi_enable_handler },
        { .uri = "/api/wifi/scan", .method = HTTP_GET,
          .handler = wifi_scan_handler },
        { .uri = "/api/wifi/connect", .method = HTTP_POST,
          .handler = wifi_connect_handler },
        { .uri = "/api/wifi/uplink", .method = HTTP_POST,
          .handler = wifi_uplink_handler },
        { .uri = "/api/wifi/*", .method = HTTP_OPTIONS,
          .handler = wifi_options_handler },
    };
    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        esp_err_t ret = httpd_register_uri_handler(s_wifi_httpd, &handlers[index]);
        if (ret != ESP_OK) {
            httpd_stop(s_wifi_httpd);
            s_wifi_httpd = NULL;
            return ret;
        }
    }
    ESP_LOGI(TAG, "Wi-Fi manager ready on SoftAP port %d; STA disabled by default",
             WIFI_MANAGER_PORT);
    return ESP_OK;
}
