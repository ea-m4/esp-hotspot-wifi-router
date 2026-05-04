#include "wifi_bridge.h"
#include "config.h"
#include "dns_filter.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_bridge";

#define WIFI_CONNECTED_BIT  BIT0
#define DHCPS_OFFER_DNS     0x02

static bool               s_sta_connected   = false;
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t       *s_netif_ap  = NULL;
static esp_netif_t       *s_netif_sta = NULL;

bool wifi_bridge_sta_connected(void)
{
    return s_sta_connected;
}

static uint32_t get_dns_ip(void)
{
    switch (g_cfg.dns_mode) {
        case DNS_CLOUDFLARE: return inet_addr("1.1.1.1");
        case DNS_ADGUARD:    return inet_addr("94.140.14.14");
        case DNS_CUSTOM:     return inet_addr(g_cfg.dns_custom);
        default: {
            esp_netif_dns_info_t d = {0};
            esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &d);
            return d.ip.u_addr.ip4.addr;
        }
    }
}

static void bridge_activate(void)
{
    uint32_t dns_ip = get_dns_ip();

    esp_netif_dns_info_t dhcp_dns = {0};
    dhcp_dns.ip.type = IPADDR_TYPE_V4;

    if (g_cfg.ad_filter) {
        /* Local proxy on 192.168.4.1:53 — point DHCP clients there */
        esp_netif_ip_info_t ap_ip;
        esp_netif_get_ip_info(s_netif_ap, &ap_ip);
        dhcp_dns.ip.u_addr.ip4.addr = ap_ip.ip.addr;
        dns_filter_start(dns_ip);
    } else {
        if (g_cfg.dns_mode == DNS_AUTO) {
            esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dhcp_dns);
        } else {
            dhcp_dns.ip.u_addr.ip4.addr = dns_ip;
        }
        dns_filter_stop();
    }

    uint8_t dhcps_dns_opt = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_netif_ap));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dhcps_dns_opt, sizeof(dhcps_dns_opt)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN, &dhcp_dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_netif_ap));

    if (esp_netif_napt_enable(s_netif_ap) != ESP_OK) {
        ESP_LOGE(TAG, "NAPT enable failed -- check CONFIG_LWIP_IPV4_NAPT=y");
    } else {
        ESP_LOGI(TAG, "Bridge ON  --  '%s' -> internet via '%s'",
                 g_cfg.ap_ssid, g_cfg.sta_ssid);
    }

    const char *dns_names[] = { "router DNS", "Cloudflare 1.1.1.1",
                                 "AdGuard DNS", "custom DNS" };
    ESP_LOGI(TAG, "DNS: %s%s", dns_names[g_cfg.dns_mode],
             g_cfg.ad_filter ? " + local filter" : "");
}

static const char *disconnect_reason_str(uint8_t r)
{
    switch (r) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE";
        case 3:   return "AUTH_LEAVE";
        case 4:   return "ASSOC_EXPIRE";
        case 8:   return "ASSOC_LEAVE";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL (wrong password?)";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT (PMF mismatch?)";
        case 205: return "CONNECTION_FAIL";
        default:  return "UNKNOWN";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch ((int)event_id) {
            case WIFI_EVENT_STA_START:
                /* Initial connect is called directly after esp_wifi_start() returns
                   in wifi_bridge_start().  Doing it here races on dual-core chips
                   (S3) because this event fires before esp_wifi_start() completes. */
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                s_sta_connected = false;
                wifi_event_sta_disconnected_t *d = event_data;
                if (strlen(g_cfg.sta_ssid) > 0) {
                    ESP_LOGW(TAG, "Upstream lost  reason=%d (%s)  -- retrying...",
                             d->reason, disconnect_reason_str(d->reason));
                    esp_wifi_connect();
                }
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = event_data;
                ESP_LOGI(TAG, "Client joined   MAC=" MACSTR "  AID=%d",
                         MAC2STR(e->mac), e->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = event_data;
                ESP_LOGI(TAG, "Client left     MAC=" MACSTR "  AID=%d",
                         MAC2STR(e->mac), e->aid);
                break;
            }
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = event_data;
        ESP_LOGI(TAG, "Got IP from upstream: " IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_connected = true;
        bridge_activate();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_tune(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT40));

#if CONFIG_SOC_WIFI_HE_SUPPORT
    wifi_protocols_t proto_he = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                  WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocols(WIFI_IF_STA, &proto_he));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocols(WIFI_IF_AP,  &proto_he));
    ESP_LOGI(TAG, "WiFi 6 (802.11ax/HE) enabled");
#endif

#if CONFIG_SOC_WIFI_SUPPORT_5G
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    wifi_protocols_t proto_5g = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                  WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N |
                  WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocols(WIFI_IF_STA, &proto_5g));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocols(WIFI_IF_AP,  &proto_5g));
    ESP_LOGI(TAG, "Dual-band (2.4 + 5 GHz) active");
#endif
}

void wifi_bridge_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* ---- Access Point ---- */
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = (uint8_t)strlen(g_cfg.ap_ssid),
            .channel        = g_cfg.ap_channel,
            .max_connection = g_cfg.ap_max_conn,
            .authmode       = WIFI_AUTH_WPA2_WPA3_PSK,
            .pmf_cfg        = { .required = false, .capable = true },
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid,     g_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, g_cfg.ap_pass,  sizeof(ap_cfg.ap.password));
    if (strlen(g_cfg.ap_pass) == 0) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_LOGI(TAG, "AP   SSID='%s'  ch=%d  auth=%s  max=%d",
             g_cfg.ap_ssid, g_cfg.ap_channel,
             strlen(g_cfg.ap_pass) ? "WPA2+WPA3" : "Open",
             g_cfg.ap_max_conn);

    /* ---- Station (upstream) ---- */
    s_netif_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_default_netif(s_netif_sta);

    wifi_config_t sta_cfg = {
        .sta = {
            .scan_method        = WIFI_ALL_CHANNEL_SCAN,
            /* Accept WPA or WPA2; don't reject routers that use mixed/legacy modes */
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            /* UNSPECIFIED: do not attempt WPA3 SAE on the upstream connection.
               On ESP32-S3 (full WPA3 HW), WPA3_SAE_PWE_BOTH causes the chip to
               initiate SAE authentication even toward WPA2-only routers that
               advertise WPA3 transition mode.  If the router's SAE stack is
               buggy the handshake hangs silently for 10-20 s before timing out.
               Forcing UNSPECIFIED makes the STA negotiate plain WPA2-PSK,
               which is what virtually every home router reliably speaks. */
            .sae_pwe_h2e        = WPA3_SAE_PWE_UNSPECIFIED,
            /* Explicit PMF=optional so the STA does not inherit PMF-required
               from the AP's WPA3 transition mode (S3-specific driver behaviour). */
            .pmf_cfg            = { .capable = true, .required = false },
        },
    };
    strlcpy((char *)sta_cfg.sta.ssid,     g_cfg.sta_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, g_cfg.sta_pass,  sizeof(sta_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_tune();

    /* Connect after esp_wifi_start() fully returns — safe on both single-core
       (ESP32) and dual-core (S3) chips.  Reconnects after drops are handled
       in WIFI_EVENT_STA_DISCONNECTED. */
    if (strlen(g_cfg.sta_ssid) > 0) {
        ESP_LOGI(TAG, "Connecting to upstream '%s'...", g_cfg.sta_ssid);
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "No upstream SSID configured -- open http://192.168.4.1");
    }
}
