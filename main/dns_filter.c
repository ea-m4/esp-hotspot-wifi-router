/*
 * DNS proxy with ad-blocking for the ESP32 WiFi bridge.
 *
 * Listens on 192.168.4.1:53 (AP interface).
 * Blocked domains  -> NXDOMAIN (instant, local)
 * Allowed domains  -> forwarded to configured upstream DNS
 *
 * Blocking is suffix-based: "doubleclick.net" matches
 * "ad.doubleclick.net", "sub.ad.doubleclick.net", etc.
 *
 * What this CANNOT block:
 *   YouTube in-stream video ads: they are served from the same
 *   domains (googlevideo.com, youtube.com) as actual video content.
 *   Use a browser extension (uBlock Origin) for YouTube.
 */

#include "dns_filter.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"

static const char *TAG = "dns_filter";

/* ====================================================================
   Blocked domain list — suffix matching, sorted alphabetically.
   Add or remove entries to customize your blocklist.
   ==================================================================== */
static const char * const BLOCKED[] = {
    "2mdn.net",               /* DoubleClick publisher network         */
    "adnxs.com",              /* AppNexus / Xandr                      */
    "adservice.google.com",   /* Google Ad Service (not search/maps)   */
    "adtech.de",              /* Adtech/TripleLift ad server           */
    "advertising.com",        /* Verizon/Oath advertising              */
    "amazon-adsystem.com",    /* Amazon display & video ads            */
    "an.facebook.com",        /* Facebook Audience Network             */
    "casalemedia.com",        /* Index Exchange SSP                    */
    "connect.facebook.net",   /* Facebook JS SDK (ads + social)        */
    "criteo.com",             /* Criteo retargeting                    */
    "criteo.net",             /* Criteo (alt domain)                   */
    "doubleclick.net",        /* Google DoubleClick ad exchange        */
    "googleadservices.com",   /* Google Ads click tracking             */
    "googlesyndication.com",  /* Google AdSense display ads            */
    "googletagservices.com",  /* Google Publisher Tags                 */
    "hotjar.com",             /* Hotjar session recording              */
    "krxd.net",               /* Salesforce Krux DMP                   */
    "moatads.com",            /* Moat viewability measurement          */
    "omnitagjs.com",          /* Outbrain Sphere ads                   */
    "openx.net",              /* OpenX ad exchange                     */
    "outbrain.com",           /* Outbrain content recommendation ads   */
    "pixel.facebook.com",     /* Facebook conversion pixel             */
    "pubmatic.com",           /* PubMatic SSP                          */
    "quantserve.com",         /* Quantcast audience measurement        */
    "revcontent.com",         /* RevContent native advertising         */
    "rfihub.com",             /* RealPage tracking                     */
    "rubiconproject.com",     /* Magnite (Rubicon) SSP                 */
    "scorecardresearch.com",  /* comScore / Nielsen measurement        */
    "sharethrough.com",       /* Sharethrough native ads               */
    "smartadserver.com",      /* Smart ad server                       */
    "sovrn.com",              /* Sovrn SSP                             */
    "spotxchange.com",        /* SpotX video advertising               */
    "taboola.com",            /* Taboola content recommendation ads    */
    "triplelift.com",         /* TripleLift native advertising         */
    "yieldmanager.com",       /* Yahoo! yield manager                  */
};
#define NUM_BLOCKED  (sizeof(BLOCKED) / sizeof(BLOCKED[0]))

/* ====================================================================
   State
   ==================================================================== */
static volatile uint32_t s_blocked_count = 0;
static volatile bool     s_running       = false;
static volatile int      s_sock          = -1;
static uint32_t          s_upstream_ip   = 0;

/* ====================================================================
   DNS helpers
   ==================================================================== */

/* Extract domain name from the QNAME field at buf[offset].
   DNS wire format: [len][label][len][label]...[0]          */
static bool extract_domain(const uint8_t *buf, int buf_len, int offset,
                            char *out, int out_len)
{
    int pos = offset, o = 0;
    bool first = true;

    while (pos < buf_len) {
        uint8_t llen = buf[pos++];
        if (llen == 0) break;
        if ((llen & 0xC0) == 0xC0) { pos++; break; }   /* pointer — skip */
        if (llen > 63 || pos + llen > buf_len) return false;
        if (!first && o < out_len - 1) out[o++] = '.';
        first = false;
        for (int i = 0; i < llen && o < out_len - 1; i++)
            out[o++] = (char)tolower((unsigned char)buf[pos + i]);
        pos += llen;
    }
    out[o] = '\0';
    return o > 0;
}

/* Suffix-based blocklist check.
   "doubleclick.net" matches "doubleclick.net" and "ad.doubleclick.net". */
static bool is_blocked(const char *domain)
{
    size_t dlen = strlen(domain);
    for (size_t i = 0; i < NUM_BLOCKED; i++) {
        size_t blen = strlen(BLOCKED[i]);
        if (dlen < blen) continue;
        size_t off = dlen - blen;
        if ((off == 0 || domain[off - 1] == '.') &&
            strcasecmp(domain + off, BLOCKED[i]) == 0)
            return true;
    }
    return false;
}

/* Forward a DNS query to the upstream server and relay the response
   back to the original client. Opens a fresh socket per query so
   concurrent requests from multiple clients don't collide.           */
static void forward_query(int client_sock, const uint8_t *query, int qlen,
                           const struct sockaddr_in *client, uint32_t upstream_ip)
{
    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (up < 0) return;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = upstream_ip,
    };
    if (sendto(up, query, qlen, 0, (struct sockaddr *)&dst, sizeof(dst)) > 0) {
        uint8_t resp[512];
        int n = (int)recv(up, resp, sizeof(resp), 0);
        if (n > 0)
            sendto(client_sock, resp, n, 0,
                   (struct sockaddr *)client, sizeof(*client));
    }
    close(up);
}

/* ====================================================================
   Proxy task
   ==================================================================== */
static void dns_proxy_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    s_sock = sock;

    /* Bind only to the AP interface IP — won't intercept STA-side traffic */
    struct sockaddr_in baddr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),
    };
    if (bind(sock, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
        ESP_LOGE(TAG, "bind(192.168.4.1:53) failed: %d", errno);
        close(sock);
        s_sock    = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS filter active on 192.168.4.1:53  |  %u domains in blocklist",
             (unsigned)NUM_BLOCKED);

    uint8_t             buf[512];
    struct sockaddr_in  caddr;
    socklen_t           clen;
    char                domain[128];

    while (s_running) {
        clen = sizeof(caddr);
        int len = (int)recvfrom(sock, buf, sizeof(buf), 0,
                                (struct sockaddr *)&caddr, &clen);
        if (len < 12) continue;
        if (buf[2] & 0x80) continue;   /* ignore responses */

        bool blocked = extract_domain(buf, len, 12, domain, sizeof(domain))
                       && is_blocked(domain);

        if (blocked) {
            s_blocked_count++;
            ESP_LOGD(TAG, "BLOCK  %s", domain);
            /* Build NXDOMAIN response in-place (same header + question) */
            buf[2] = 0x81; buf[3] = 0x83;  /* QR AA RD RA, RCODE=NXDOMAIN */
            buf[6] = 0; buf[7] = 0;         /* ANCOUNT = 0 */
            buf[8] = 0; buf[9] = 0;         /* NSCOUNT = 0 */
            buf[10] = 0; buf[11] = 0;       /* ARCOUNT = 0 */
            sendto(sock, buf, len, 0, (struct sockaddr *)&caddr, clen);
        } else {
            forward_query(sock, buf, len, &caddr, s_upstream_ip);
        }
    }

    close(sock);
    s_sock = -1;
    vTaskDelete(NULL);
}

/* ====================================================================
   Public API
   ==================================================================== */

void dns_filter_start(uint32_t upstream_ip)
{
    if (s_running) {
        /* Already running — update upstream IP and continue */
        s_upstream_ip = upstream_ip;
        return;
    }
    s_upstream_ip   = upstream_ip;
    s_blocked_count = 0;
    s_running       = true;
    xTaskCreate(dns_proxy_task, "dns_proxy", 4096, NULL, 5, NULL);
}

void dns_filter_stop(void)
{
    if (!s_running) return;
    s_running = false;
    /* Closing the socket unblocks the recvfrom() in the task */
    int fd = s_sock;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

bool dns_filter_running(void)
{
    return s_running;
}

uint32_t dns_filter_blocked_count(void)
{
    return s_blocked_count;
}
