/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "rom/ets_sys.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"
#include "app_wifi.h"

/* The examples use WiFi configuration that you can set via 'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY
#define EXAMPLE_IP_ADDR            CONFIG_SERVER_IP

static const char *TAG = "app wifi";

static int s_retry_num = 0;
static esp_netif_t *wifi_ap_netif = NULL;
static esp_netif_t *wifi_sta_netif = NULL;
static SemaphoreHandle_t s_wifi_lock = NULL;
static EventGroupHandle_t s_wifi_events = NULL;
static bool s_wifi_initialized = false;
static bool s_wifi_driver_started = false;
static bool s_softap_started = false;

#define SOFTAP_STARTED_BIT BIT0
#define SOFTAP_STOPPED_BIT BIT1
#define SOFTAP_TRANSITION_TIMEOUT_MS 5000

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        if (s_wifi_events != NULL) {
            xEventGroupClearBits(s_wifi_events, SOFTAP_STOPPED_BIT);
            xEventGroupSetBits(s_wifi_events, SOFTAP_STARTED_BIT);
        }
        ESP_LOGI(TAG, "SoftAP start event received");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        if (s_wifi_events != NULL) {
            xEventGroupClearBits(s_wifi_events, SOFTAP_STARTED_BIT);
            xEventGroupSetBits(s_wifi_events, SOFTAP_STOPPED_BIT);
        }
        ESP_LOGI(TAG, "SoftAP stop event received");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);

        ESP_LOGI(TAG, "station traffic will be routed through L610 RNDIS");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
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

static esp_err_t wifi_init_softap(esp_netif_t *wifi_netif, const modem_wifi_config_t *config)
{
    if (strcmp(EXAMPLE_IP_ADDR, "192.168.4.1")) {
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(esp_netif_ip_info_t));
        ip.ip.addr = ipaddr_addr(EXAMPLE_IP_ADDR);
        ip.gw.addr = ipaddr_addr(EXAMPLE_IP_ADDR);
        ip.netmask.addr = ipaddr_addr("255.255.255.0");
        ESP_RETURN_ON_ERROR(set_dhcps_running(wifi_netif, false), TAG,
                            "Failed to stop SoftAP DHCP server");
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(wifi_netif, &ip), TAG,
                            "Failed to configure SoftAP address");
    }
    // Offer the DNS learned from the L610 RNDIS DHCP server to SoftAP clients.
    esp_netif_dns_info_t dns;
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
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(wifi_netif, ESP_NETIF_DNS_MAIN, &dns),
                        TAG, "Failed to configure SoftAP DNS");
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    snprintf((char*)wifi_config.ap.ssid, 32, "%s", config->ap_ssid);
    wifi_config.ap.ssid_len = strlen((char*)wifi_config.ap.ssid);
    snprintf((char*)wifi_config.ap.password, 64, "%s", config->ap_password);
    wifi_config.ap.max_connection = config->max_connection;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen(config->ap_password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.ap.channel = config->channel;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config), TAG,
                        "Failed to configure SoftAP");

    ESP_LOGI(TAG, "SoftAP configured: SSID=%s, DNS=%s", config->ap_ssid, dns_addr);
    return ESP_OK;
}

void wifi_init_sta(esp_netif_t *wifi_netif, const modem_wifi_config_t *config)
{
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    snprintf((char*)wifi_config.sta.ssid, 32, "%s", config->sta_ssid);
    snprintf((char*)wifi_config.sta.password, 64, "%s", config->sta_password);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s",
             config->sta_ssid, config->sta_password);
}

static esp_err_t initialize_wifi_once(const modem_wifi_config_t *config)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS initialization failed");

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (config->mode == WIFI_MODE_APSTA) {
        mode = WIFI_MODE_APSTA;
        wifi_ap_netif = esp_netif_create_default_wifi_ap();
        wifi_sta_netif = esp_netif_create_default_wifi_sta();
    } else if (config->mode == WIFI_MODE_AP) {
        mode = WIFI_MODE_AP;
        wifi_ap_netif = esp_netif_create_default_wifi_ap();
    } else if (config->mode == WIFI_MODE_STA) {
        mode = WIFI_MODE_STA;
        wifi_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (mode == WIFI_MODE_NULL) {
        ESP_LOGW(TAG, "Neither AP or STA have been configured. WiFi will be off.");
        return ESP_ERR_INVALID_ARG;
    }
    if ((mode & WIFI_MODE_AP) && wifi_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if ((mode & WIFI_MODE_STA) && wifi_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to create SoftAP lifecycle events");
        xEventGroupSetBits(s_wifi_events, SOFTAP_STOPPED_BIT);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL),
        TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL),
        TAG, "IP event registration failed");
    /* Runtime toggles must not rewrite flash on every command. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Wi-Fi storage configuration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "Wi-Fi mode configuration failed");

    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t stop_softap_locked(void)
{
    esp_err_t first_error = ESP_OK;
    if (wifi_ap_netif != NULL) {
        esp_err_t ret = esp_netif_napt_disable(wifi_ap_netif);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable SoftAP NAPT: %s", esp_err_to_name(ret));
            first_error = ret;
        }
        ret = set_dhcps_running(wifi_ap_netif, false);
        if (ret != ESP_OK && first_error == ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop SoftAP DHCP server: %s", esp_err_to_name(ret));
            first_error = ret;
        }
    }

    if (s_wifi_driver_started) {
        const bool was_up = wifi_ap_netif != NULL && esp_netif_is_netif_up(wifi_ap_netif);
        xEventGroupClearBits(s_wifi_events, SOFTAP_STOPPED_BIT);
        esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to disable SoftAP mode: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        } else if (was_up) {
            ret = wait_for_softap_event(SOFTAP_STOPPED_BIT, "stop");
            if (ret != ESP_OK && first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }

    if (wifi_ap_netif == NULL || !esp_netif_is_netif_up(wifi_ap_netif)) {
        s_softap_started = false;
    }
    return first_error;
}

esp_err_t app_wifi_start_softap(const modem_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && (config->mode & WIFI_MODE_AP),
                        ESP_ERR_INVALID_ARG, TAG, "SoftAP configuration is required");
    if (s_wifi_lock == NULL) {
        s_wifi_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_wifi_lock != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to create Wi-Fi control lock");
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_wifi_lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Failed to lock Wi-Fi control");

    esp_err_t ret = ESP_OK;
    if (s_softap_started) {
        goto done;
    }
    ret = initialize_wifi_once(config);
    if (ret != ESP_OK) {
        goto done;
    }

    xEventGroupClearBits(s_wifi_events, SOFTAP_STARTED_BIT);
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select SoftAP mode: %s", esp_err_to_name(ret));
        goto done;
    }
    ret = wifi_init_softap(wifi_ap_netif, config);
    if (ret != ESP_OK) {
        goto done;
    }

    if (!s_wifi_driver_started) {
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Wi-Fi driver: %s", esp_err_to_name(ret));
            goto done;
        }
        s_wifi_driver_started = true;
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
        ESP_LOGE(TAG, "Failed to start SoftAP DHCP server: %s", esp_err_to_name(ret));
        goto rollback;
    }
    ret = esp_wifi_set_bandwidth(ESP_IF_WIFI_AP, config->bandwidth);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SoftAP bandwidth: %s", esp_err_to_name(ret));
        goto rollback;
    }
    esp_netif_ip_info_t ip;
    ret = esp_netif_get_ip_info(wifi_ap_netif, &ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SoftAP address: %s", esp_err_to_name(ret));
        goto rollback;
    }
    /* Bind NAPT to the actual SoftAP netif. */
    ret = esp_netif_napt_enable(wifi_ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable SoftAP NAPT: %s", esp_err_to_name(ret));
        goto rollback;
    }
    s_softap_started = true;
    ESP_LOGI(TAG, "NAPT enabled on SoftAP " IPSTR, IP2STR(&ip.ip));
    goto done;

rollback:
    {
        esp_err_t operation_error = ret;
        esp_err_t rollback_error = stop_softap_locked();
        if (operation_error == ESP_OK) {
            ret = rollback_error;
        } else {
            ret = operation_error;
        }
    }
done:
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
    if (!s_softap_started) {
        xSemaphoreGive(s_wifi_lock);
        return ESP_OK;
    }

    esp_err_t stop_ret = stop_softap_locked();
    if (!s_softap_started) {
        ESP_LOGI(TAG, "SoftAP/NAPT stopped");
    }
    xSemaphoreGive(s_wifi_lock);
    return stop_ret;
}

bool app_wifi_softap_is_started(void)
{
    return s_softap_started;
}

esp_netif_t *app_wifi_get_sta_netif(void)
{
    return wifi_sta_netif;
}

esp_netif_t *app_wifi_get_ap_netif(void)
{
    return wifi_ap_netif;
}
