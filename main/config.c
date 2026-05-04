#include "config.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config";

bridge_config_t g_cfg;

void config_load(bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->ap_ssid,    DEFAULT_AP_SSID, sizeof(cfg->ap_ssid));
    strlcpy(cfg->ap_pass,    DEFAULT_AP_PASS, sizeof(cfg->ap_pass));
    strlcpy(cfg->dns_custom, "8.8.8.8",       sizeof(cfg->dns_custom));
    cfg->ap_channel  = DEFAULT_AP_CHANNEL;
    cfg->ap_max_conn = DEFAULT_AP_MAX_CONN;
    cfg->dns_mode    = DNS_AUTO;
    cfg->ad_filter   = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return;

    size_t len;
    len = sizeof(cfg->sta_ssid);   nvs_get_str(nvs, "sta_ssid",   cfg->sta_ssid,   &len);
    len = sizeof(cfg->sta_pass);   nvs_get_str(nvs, "sta_pass",   cfg->sta_pass,   &len);
    len = sizeof(cfg->ap_ssid);    nvs_get_str(nvs, "ap_ssid",    cfg->ap_ssid,    &len);
    len = sizeof(cfg->ap_pass);    nvs_get_str(nvs, "ap_pass",    cfg->ap_pass,    &len);
    len = sizeof(cfg->dns_custom); nvs_get_str(nvs, "dns_custom", cfg->dns_custom, &len);
    uint8_t v;
    if (nvs_get_u8(nvs, "ap_chan",    &v) == ESP_OK) cfg->ap_channel  = v;
    if (nvs_get_u8(nvs, "ap_max",    &v) == ESP_OK) cfg->ap_max_conn = v;
    if (nvs_get_u8(nvs, "dns_mode",  &v) == ESP_OK) cfg->dns_mode    = v;
    if (nvs_get_u8(nvs, "ad_filter", &v) == ESP_OK) cfg->ad_filter   = v;
    nvs_close(nvs);
}

void config_save(const bridge_config_t *cfg)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed");
        return;
    }
    nvs_set_str(nvs, "sta_ssid",   cfg->sta_ssid);
    nvs_set_str(nvs, "sta_pass",   cfg->sta_pass);
    nvs_set_str(nvs, "ap_ssid",    cfg->ap_ssid);
    nvs_set_str(nvs, "ap_pass",    cfg->ap_pass);
    nvs_set_str(nvs, "dns_custom", cfg->dns_custom);
    nvs_set_u8(nvs,  "ap_chan",    cfg->ap_channel);
    nvs_set_u8(nvs,  "ap_max",     cfg->ap_max_conn);
    nvs_set_u8(nvs,  "dns_mode",   cfg->dns_mode);
    nvs_set_u8(nvs,  "ad_filter",  cfg->ad_filter);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config saved to NVS");
}
