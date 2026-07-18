/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define WIFI_BW_HT20 WIFI_BW20
#define WIFI_BW_HT40 WIFI_BW40

#define ESP_IF_WIFI_STA WIFI_IF_STA
#define ESP_IF_WIFI_AP WIFI_IF_AP
#endif

#ifdef CONFIG_WIFI_BANDWIFTH_40
#define MODEM_WIFI_BANDWIFTH WIFI_BW_HT40
#else
#define MODEM_WIFI_BANDWIFTH WIFI_BW_HT20
#endif

/**
 * @brief  Broadcast SSID or not, default 0, broadcast the SSID
 *
 */
#define MODEM_WIFI_DEFAULT_CONFIG()                \
{                                                  \
    .mode = WIFI_MODE_AP,                          \
    .ap_ssid = CONFIG_ESP_WIFI_AP_SSID,            \
    .ap_password = CONFIG_ESP_WIFI_AP_PASSWORD,    \
    .sta_ssid = CONFIG_ESP_WIFI_STA_SSID,          \
    .sta_password = CONFIG_ESP_WIFI_STA_PASSWORD,  \
    .channel = CONFIG_ESP_WIFI_AP_CHANNEL,         \
    .max_connection = CONFIG_MODEM_WIFI_MAX_STA,   \
    .ssid_hidden = 0,                              \
    .authmode = WIFI_AUTH_WPA_WPA2_PSK,            \
    .bandwidth = MODEM_WIFI_BANDWIFTH,             \
}

typedef struct {
    wifi_mode_t mode;              /*!< Wi-Fi Work mode */
    char ap_ssid[33];                 /*!< Wi-Fi SSID of AP mode */
    char ap_password[65];             /*!< Wi-Fi password of AP mode */
    char sta_ssid[33];             /*!< Wi-Fi SSID for station mode*/
    char sta_password[65];         /*!< Wi-Fi password for station mode*/
    char dns[16];                  /*!< Wi-Fi SoftAP DNS address */
    size_t channel;                /*!< Wi-Fi channel of the mode */
    size_t max_connection;         /*!< Wi-Fi max connections of the softap mode */
    size_t ssid_hidden;            /*!< If hide ssid in softap mode */
    wifi_auth_mode_t authmode;     /*!< Wi-Fi authenticate  mode */
    wifi_bandwidth_t bandwidth;    /*!< Wi-Fi bandwidth 20MHz or 40MHz */
} modem_wifi_config_t;

typedef esp_err_t (*app_wifi_uplink_callback_t)(bool use_wifi, bool connected,
                                                esp_netif_t *sta_netif, void *ctx);

#define APP_WIFI_MAX_SCAN_RESULTS 20

typedef enum {
    APP_WIFI_UPLINK_NONE = 0,
    APP_WIFI_UPLINK_L610,
    APP_WIFI_UPLINK_WIFI,
} app_wifi_active_uplink_t;

typedef struct {
    char ssid[33];
    int rssi;
    int channel;
    char security[16];
    bool secured;
    bool supported;
} app_wifi_network_t;

typedef struct {
    bool feature_enabled;
    bool scanning;
    bool connected;
    char ssid[33];
    char ip[16];
    bool rssi_valid;
    int rssi;
    bool wifi_uplink_selected;
    app_wifi_active_uplink_t active_uplink;
    char error[96];
} app_wifi_status_t;

/**
 * @brief Start SoftAP and NAPT internet sharing.
 *
 * The Wi-Fi driver and AP netif are initialized once. Repeated calls are
 * idempotent so cloud control can safely retry a command.
 */
esp_err_t app_wifi_start_softap(const modem_wifi_config_t *config);

/**
 * @brief Stop SoftAP and NAPT internet sharing.
 *
 * The driver and netif remain initialized so the AP can be enabled again.
 */
esp_err_t app_wifi_stop_softap(void);

/**
 * @brief Return whether SoftAP and NAPT completed startup.
 */
bool app_wifi_softap_is_started(void);

/** Register the main application callback that rebinds cloud sockets. */
void app_wifi_set_uplink_callback(app_wifi_uplink_callback_t callback,
                                  void *ctx);

/** Re-evaluate the selected uplink after the L610 link changes. */
esp_err_t app_wifi_refresh_uplink(void);

/** True only when STA is connected and selected as the cloud uplink. */
bool app_wifi_sta_uplink_is_active(void);

/**
 * @brief Get the wifi station netif
 *
 * @return The station netif pointer
 */
esp_netif_t *app_wifi_get_sta_netif(void);

/**
 * @brief Get the wifi softap netif
 *
 * @return The softap netif pointer
 */
esp_netif_t *app_wifi_get_ap_netif(void);

/** Thread-safe Wi-Fi STA controls used by both local HTTP and cloud workers. */
esp_err_t app_wifi_set_sta_feature_enabled(bool enabled);
esp_err_t app_wifi_scan_networks(app_wifi_network_t *networks,
                                 size_t capacity, size_t *count);
esp_err_t app_wifi_connect_sta(const char *ssid, const char *password,
                               const char *security);
esp_err_t app_wifi_select_cloud_uplink(bool use_wifi);
esp_err_t app_wifi_get_status(app_wifi_status_t *status);

#ifdef __cplusplus
}
#endif
