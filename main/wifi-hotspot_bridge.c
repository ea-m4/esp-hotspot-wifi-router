/*
 * WiFi Hotspot Bridge for ESP32
 *
 * Connects to an upstream WiFi network (STA) and shares the internet
 * through its own Access Point, with NAT, DNS selection, and ad filtering.
 *
 * WiFi generation by chip:
 *   ESP32 / S2 / S3 / C3  ->  WiFi 4 (802.11n),        2.4 GHz only
 *   ESP32-C6 / C61         ->  WiFi 6 (802.11ax / HE),  2.4 GHz only
 *   ESP32-C5               ->  WiFi 6 (802.11ax / HE),  2.4 GHz + 5 GHz
 *
 * Topology:
 *   [Phone/PC] --WiFi--> [ESP32 AP] --NAT--> [ESP32 STA] --WiFi--> [Router] --> Internet
 *
 * Configuration: connect to the AP and open http://192.168.4.1
 */

#include "config.h"
#include "wifi_bridge.h"
#include "web_config.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "main";

void app_main(void)
{
#if CONFIG_SOC_WIFI_SUPPORT_5G
    ESP_LOGI(TAG, "Chip: WiFi 6 (802.11ax)  |  2.4 GHz + 5 GHz");
#elif CONFIG_SOC_WIFI_HE_SUPPORT
    ESP_LOGI(TAG, "Chip: WiFi 6 (802.11ax)  |  2.4 GHz only");
#else
    ESP_LOGI(TAG, "Chip: WiFi 4 (802.11n)   |  2.4 GHz only");
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    config_load(&g_cfg);
    ESP_LOGI(TAG, "Config: AP='%s'  STA='%s'  DNS=%s%s",
             g_cfg.ap_ssid,
             strlen(g_cfg.sta_ssid) ? g_cfg.sta_ssid : "(not set)",
             (const char *[]){"auto","cloudflare","adguard","custom"}[g_cfg.dns_mode],
             g_cfg.ad_filter ? "+filter" : "");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_bridge_start();
    webserver_start();
}
