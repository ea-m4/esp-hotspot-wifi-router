#include "web_config.h"
#include "config.h"
#include "wifi_bridge.h"
#include "dns_filter.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "lwip/inet.h"

static const char *TAG = "web_config";

/* ====================================================================
   Form helpers
   ==================================================================== */

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t i = 0;
    while (*src && i < dst_len - 1) {
        if (*src == '+') {
            dst[i++] = ' '; src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void parse_field(const char *body, const char *key, char *out, size_t out_len)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char tmp[256] = {0};
    if (len > sizeof(tmp) - 1) len = sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    url_decode(out, tmp, out_len);
}

static int parse_int_field(const char *body, const char *key, int def)
{
    char buf[8];
    parse_field(body, key, buf, sizeof(buf));
    if (buf[0] == '\0') return def;
    int v = atoi(buf);
    return (v > 0) ? v : def;
}

/* ====================================================================
   WiFi scan endpoint  GET /scan
   Returns JSON: [{"s":"SSID","r":-65,"o":false}, ...]
     s = ssid, r = rssi, o = open (true = no password)
   ==================================================================== */

#define SCAN_MAX 20

static void json_escape_ssid(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (*src && i < dst_len - 2) {
        unsigned char c = (unsigned char)*src++;
        if      (c == '"')  { dst[i++] = '\\'; dst[i++] = '"';  }
        else if (c == '\\') { dst[i++] = '\\'; dst[i++] = '\\'; }
        else if (c >= 0x20) { dst[i++] = c; }
    }
    dst[i] = '\0';
}

static esp_err_t handler_get_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden          = false,
        .scan_type            = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scan_cfg, true);   /* blocking ~1-2 s */

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found > SCAN_MAX) found = SCAN_MAX;

    wifi_ap_record_t *aps = malloc(found * sizeof(wifi_ap_record_t));
    if (!aps) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&found, aps);

    /* Each entry: {"s":"<32-char-ssid>","r":-100,"o":false} ≤ 70 chars */
    char *buf = malloc((size_t)found * 72 + 4);
    if (!buf) {
        free(aps);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }
    char esc[68];
    int  pos   = 0;
    bool first = true;
    buf[pos++] = '[';
    for (uint16_t i = 0; i < found; i++) {
        json_escape_ssid((char *)aps[i].ssid, esc, sizeof(esc));
        if (esc[0] == '\0') continue;
        if (!first) buf[pos++] = ',';
        first = false;
        pos += snprintf(buf + pos, 72, "{\"s\":\"%s\",\"r\":%d,\"o\":%s}",
                        esc, aps[i].rssi,
                        aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    buf[pos++] = ']';
    buf[pos]   = '\0';
    free(aps);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ====================================================================
   HTML template
   Format args (in order):
    1  %s  status_class ("online" / "offline")
    2  %s  status_text
    3  %s  sta_ssid          (pre-fills text input)
    4  %s  sta_pass
    5  %s  ap_ssid
    6  %s  ap_pass
    7  %d  ap_channel
    8  %d  ap_max_conn
    9  %s  dns_opt0 "selected" or ""
   10  %s  dns_opt1 "selected" or ""
   11  %s  dns_opt2 "selected" or ""
   12  %s  dns_opt3 "selected" or ""
   13  %s  custom_dns_display ("block" or "none")
   14  %s  dns_custom value
   15  %s  ad_filter "checked" or ""
   16  %lu blocked_count
   ==================================================================== */
static const char HTML_TMPL[] =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Bridge Config</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#f0f2f5;color:#333;padding:20px 16px}"
    "h1{font-size:1.35em;font-weight:700;margin-bottom:2px}"
    ".sub{color:#999;font-size:.82em;margin-bottom:18px}"
    ".chip{display:inline-block;background:#e3f2fd;color:#1565c0;font-size:.72em;"
    "padding:2px 8px;border-radius:20px;font-weight:600;margin-left:6px;"
    "vertical-align:middle}"
    ".status{display:flex;align-items:center;gap:8px;padding:10px 14px;"
    "border-radius:10px;margin-bottom:18px;font-size:.88em;font-weight:500}"
    ".online{background:#e8f5e9;color:#2e7d32}"
    ".offline{background:#fbe9e7;color:#bf360c}"
    ".dot{width:9px;height:9px;border-radius:50%%;background:currentColor;flex-shrink:0}"
    ".card{background:#fff;border-radius:14px;padding:18px;margin-bottom:14px;"
    "box-shadow:0 1px 4px rgba(0,0,0,.07)}"
    "h3{font-size:.75em;color:#aaa;text-transform:uppercase;letter-spacing:.07em;"
    "font-weight:700;margin-bottom:14px}"
    "label{font-size:.83em;color:#666;display:block;margin-top:12px;margin-bottom:4px}"
    "label:first-of-type{margin-top:0}"
    "input[type=text],input[type=password],input[type=number],select{"
    "width:100%%;padding:10px 12px;border:1.5px solid #e8e8e8;"
    "border-radius:8px;font-size:.95em;background:#fafafa;"
    "transition:border-color .15s,background .15s}"
    "input:focus,select:focus{outline:none;border-color:#4CAF50;background:#fff}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:12px}"
    ".chk{display:flex;align-items:flex-start;gap:8px;cursor:pointer;"
    "font-size:.88em;color:#555;margin-top:14px;line-height:1.5}"
    ".chk input[type=checkbox]{width:auto;margin-top:2px;flex-shrink:0;cursor:pointer}"
    ".hint{font-size:.78em;color:#aaa;margin-top:10px;line-height:1.6;"
    "padding:8px 10px;background:#fafafa;border-radius:6px;"
    "border-left:3px solid #e0e0e0}"
    ".stat{font-size:.78em;color:#4CAF50;margin-top:8px;font-weight:500}"
    "button{width:100%%;padding:13px;background:#4CAF50;color:#fff;border:none;"
    "border-radius:10px;font-size:1em;font-weight:600;cursor:pointer;"
    "margin-top:8px;transition:background .15s,transform .1s}"
    "button:active{background:#388E3C;transform:scale(.98)}"
    /* Scan refresh button overrides the full-width save-button defaults */
    "#rb{width:auto;padding:3px 10px;font-size:.77em;background:#e8f5e9;"
    "color:#2e7d32;border:1px solid #a5d6a7;border-radius:6px;margin:0}"
    "#rb:active{background:#c8e6c9;transform:scale(.97)}"
    "#rb:disabled{opacity:.5;cursor:default}"
    ".note{font-size:.77em;color:#bbb;text-align:center;margin-top:10px;line-height:1.5}"
    ".ip{font-size:.78em;color:#ccc;margin-top:14px;text-align:center}"
    "</style></head><body>"
    "<h1>ESP32 Bridge "
#if CONFIG_SOC_WIFI_SUPPORT_5G
    "<span class='chip'>WiFi 6 + 5 GHz</span>"
#elif CONFIG_SOC_WIFI_HE_SUPPORT
    "<span class='chip'>WiFi 6</span>"
#else
    "<span class='chip'>WiFi 4</span>"
#endif
    "</h1>"
    "<p class='sub'>Hotspot bridge configuration</p>"
    "<div class='status %s'><div class='dot'></div><span>%s</span></div>"
    "<form method='POST' action='/save'>"

    /* ---- Upstream WiFi ---- */
    "<div class='card'>"
    "<h3>Upstream WiFi (internet source)</h3>"
    "<label style='display:flex;justify-content:space-between;align-items:center'>"
    "Available networks"
    "<button type='button' id='rb' onclick='doScan()'>&#x21BB; Scan</button>"
    "</label>"
    "<select id='sl' onchange='onPick(this)'>"
    "<option>Scanning...</option>"
    "</select>"
    "<label>SSID <small style='color:#bbb;font-weight:400'>(or type manually)</small></label>"
    "<input type='text' id='si' name='sta_ssid' value='%s'"
    " placeholder='Select above or type SSID' autocomplete='off' required>"
    "<label>Password</label>"
    "<input type='password' name='sta_pass' value='%s'"
    " placeholder='Router password' autocomplete='new-password'>"
    "</div>"

    /* ---- Hotspot ---- */
    "<div class='card'>"
    "<h3>Hotspot &mdash; this device</h3>"
    "<label>Hotspot name</label>"
    "<input type='text' name='ap_ssid' value='%s' placeholder='ESP32-Bridge' required>"
    "<label>Password <small>(min 8 chars &mdash; empty = open network)</small></label>"
    "<input type='password' name='ap_pass' value='%s' autocomplete='new-password'>"
    "<div class='row'>"
    "<div><label>Channel (1-13)</label>"
    "<input type='number' name='ap_chan' value='%d' min='1' max='13'></div>"
    "<div><label>Max clients</label>"
    "<input type='number' name='ap_max' value='%d' min='1' max='10'></div>"
    "</div></div>"

    /* ---- DNS & Ad Blocking ---- */
    "<div class='card'>"
    "<h3>DNS &amp; Ad Blocking</h3>"
    "<label>DNS Server</label>"
    "<select name='dns_mode' id='dm' onchange='upd()'>"
    "<option value='0' %s>Router DNS (automatic)</option>"
    "<option value='1' %s>Cloudflare 1.1.1.1 &mdash; fast &amp; private</option>"
    "<option value='2' %s>AdGuard DNS &mdash; blocks ads &amp; trackers</option>"
    "<option value='3' %s>Custom DNS server</option>"
    "</select>"
    "<div id='cr' style='display:%s'>"
    "<label>Custom DNS IP address</label>"
    "<input type='text' name='dns_custom' value='%s' placeholder='e.g. 94.140.14.14'>"
    "</div>"
    "<label class='chk'>"
    "<input type='checkbox' name='ad_filter' value='1' %s>"
    "<span>Enable local ad filter &mdash; blocks ~35 ad network domains on-device</span>"
    "</label>"
    "<p class='hint'>"
    "<strong>AdGuard DNS</strong> or local filter block banner ads, trackers &amp; pop-ups "
    "on most websites.<br>"
    "<strong>YouTube in-stream video ads</strong> cannot be blocked via DNS &mdash; "
    "YouTube serves ads from the same servers as videos. "
    "Use <em>uBlock Origin</em> browser extension for YouTube."
    "</p>"
    "<p class='stat'>Blocked queries this session: %lu</p>"
    "</div>"

    "<button type='submit'>&#x1F4BE;&nbsp; Save &amp; Restart</button>"
    "<p class='note'>"
    "ESP32 will restart and apply new settings.<br>"
    "Reconnect to the hotspot, then re-open this page."
    "</p>"
    "</form>"
    "<p class='ip'>http://192.168.4.1</p>"
    "<script>"
    /* DNS custom field show/hide */
    "function upd(){document.getElementById('cr').style.display="
    "document.getElementById('dm').value==='3'?'block':'none';}"
    /* Copy selected network name into the SSID text input */
    "function onPick(s){if(s.value)document.getElementById('si').value=s.value;}"
    /* Scan and populate the dropdown */
    "function doScan(){"
    "var sel=document.getElementById('sl'),btn=document.getElementById('rb');"
    "sel.options.length=0;"
    "var ld=document.createElement('option');ld.text='Scanning...';sel.add(ld);"
    "btn.disabled=true;"
    "fetch('/scan').then(function(r){return r.json();})"
    ".then(function(aps){"
    "sel.options.length=0;"
    "var d=document.createElement('option');d.value='';d.text='-- select network --';sel.add(d);"
    "var cur=document.getElementById('si').value;"
    "aps.forEach(function(n){"
    "var o=document.createElement('option');"
    "o.value=n.s;"
    /* Signal bars using Unicode filled/empty circles: ● = ●, ○ = ○ */
    "o.text=n.s+' '+(n.r>-55?'\\u25cf\\u25cf\\u25cf\\u25cf'"
    ":n.r>-65?'\\u25cf\\u25cf\\u25cf\\u25cb'"
    ":n.r>-75?'\\u25cf\\u25cf\\u25cb\\u25cb'"
    ":'\\u25cf\\u25cb\\u25cb\\u25cb');"
    "if(n.s===cur)o.selected=true;"
    "sel.add(o);"
    "});"
    "btn.disabled=false;"
    "}).catch(function(){"
    "sel.options.length=0;"
    "var e=document.createElement('option');e.text='Scan failed - type SSID below';sel.add(e);"
    "btn.disabled=false;"
    "});}"
    "upd();doScan();"
    "</script>"
    "</body></html>";

/* ====================================================================
   HTTP handlers
   ==================================================================== */

static esp_err_t handler_get_root(httpd_req_t *req)
{
    char *buf = malloc(8192);
    if (!buf) return ESP_ERR_NO_MEM;

    const char *cls = wifi_bridge_sta_connected() ? "online" : "offline";
    const char *txt = wifi_bridge_sta_connected()
        ? "Connected to upstream WiFi"
        : (strlen(g_cfg.sta_ssid) == 0
           ? "Not configured &mdash; fill in upstream WiFi below"
           : "Connecting to upstream WiFi...");

    snprintf(buf, 8192, HTML_TMPL,
        /* status   */ cls, txt,
        /* upstream */ g_cfg.sta_ssid, g_cfg.sta_pass,
        /* hotspot  */ g_cfg.ap_ssid,  g_cfg.ap_pass,
                       (int)g_cfg.ap_channel, (int)g_cfg.ap_max_conn,
        /* dns opts */ g_cfg.dns_mode == DNS_AUTO       ? "selected" : "",
                       g_cfg.dns_mode == DNS_CLOUDFLARE ? "selected" : "",
                       g_cfg.dns_mode == DNS_ADGUARD    ? "selected" : "",
                       g_cfg.dns_mode == DNS_CUSTOM     ? "selected" : "",
        /* custom   */ g_cfg.dns_mode == DNS_CUSTOM ? "block" : "none",
                       g_cfg.dns_custom,
        /* filter   */ g_cfg.ad_filter ? "checked" : "",
        /* stats    */ (unsigned long)dns_filter_blocked_count());

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t handler_post_save(httpd_req_t *req)
{
    char body[700] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    bridge_config_t nc;
    memset(&nc, 0, sizeof(nc));
    parse_field(body, "sta_ssid",   nc.sta_ssid,   sizeof(nc.sta_ssid));
    parse_field(body, "sta_pass",   nc.sta_pass,   sizeof(nc.sta_pass));
    parse_field(body, "ap_ssid",    nc.ap_ssid,    sizeof(nc.ap_ssid));
    parse_field(body, "ap_pass",    nc.ap_pass,    sizeof(nc.ap_pass));
    parse_field(body, "dns_custom", nc.dns_custom, sizeof(nc.dns_custom));
    nc.ap_channel  = (uint8_t)parse_int_field(body, "ap_chan",  DEFAULT_AP_CHANNEL);
    nc.ap_max_conn = (uint8_t)parse_int_field(body, "ap_max",   DEFAULT_AP_MAX_CONN);
    nc.dns_mode    = (uint8_t)parse_int_field(body, "dns_mode", DNS_AUTO);

    char chk[4] = {0};
    parse_field(body, "ad_filter", chk, sizeof(chk));
    nc.ad_filter = (chk[0] == '1') ? 1 : 0;

    if (strlen(nc.ap_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Hotspot SSID cannot be empty");
        return ESP_OK;
    }
    if (strlen(nc.ap_pass) > 0 && strlen(nc.ap_pass) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Hotspot password must be 8+ chars or empty (open network)");
        return ESP_OK;
    }
    if (nc.dns_mode > 3) nc.dns_mode = DNS_AUTO;
    if (nc.dns_mode == DNS_CUSTOM && inet_addr(nc.dns_custom) == INADDR_NONE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid custom DNS IP address");
        return ESP_OK;
    }
    if (nc.ap_channel < 1  || nc.ap_channel > 13) nc.ap_channel  = DEFAULT_AP_CHANNEL;
    if (nc.ap_max_conn < 1 || nc.ap_max_conn > 10) nc.ap_max_conn = DEFAULT_AP_MAX_CONN;

    config_save(&nc);

    static const char SAVED_PAGE[] =
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>body{font-family:-apple-system,sans-serif;text-align:center;"
        "padding:50px 20px;background:#f0f2f5;color:#333}"
        ".icon{font-size:3.5em;margin-bottom:12px}"
        "h2{margin-bottom:10px}p{color:#888;font-size:.9em;line-height:1.7}"
        "</style></head><body>"
        "<div class='icon'>&#x2705;</div>"
        "<h2>Settings saved!</h2>"
        "<p>Restarting with new settings&hellip;<br>"
        "Reconnect to the hotspot if needed,<br>"
        "then re-open <strong>http://192.168.4.1</strong></p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_PAGE, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

/* ====================================================================
   Public API
   ==================================================================== */

void webserver_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handler_get_root
    };
    static const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = handler_get_scan
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = handler_post_save
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_save);

    ESP_LOGI(TAG, "Config page  ->  http://192.168.4.1");
}
