#pragma once
#include <stdint.h>

/* ---- Factory defaults (used on first boot, before any web config) ---- */
#define DEFAULT_AP_SSID     "ESP32-Bridge"
#define DEFAULT_AP_PASS     "bridge123"
#define DEFAULT_AP_CHANNEL  1
#define DEFAULT_AP_MAX_CONN 5

/* ---- NVS namespace ---------------------------------------------------- */
#define NVS_NS              "bridge_cfg"

/* ---- DNS mode values -------------------------------------------------- */
#define DNS_AUTO        0   /* use DNS assigned by upstream router */
#define DNS_CLOUDFLARE  1   /* 1.1.1.1  — fast & private          */
#define DNS_ADGUARD     2   /* 94.140.14.14 — blocks ads/trackers  */
#define DNS_CUSTOM      3   /* user-defined IP                     */

/* ---- Shared config struct --------------------------------------------- */
typedef struct {
    char    sta_ssid[33];   /* upstream (internet) WiFi SSID       */
    char    sta_pass[65];   /* upstream WiFi password               */
    char    ap_ssid[33];    /* this device's AP name               */
    char    ap_pass[65];    /* AP password (empty = open network)  */
    uint8_t ap_channel;     /* AP channel 1-13                     */
    uint8_t ap_max_conn;    /* max simultaneous AP clients         */
    uint8_t dns_mode;       /* DNS_AUTO / CLOUDFLARE / ADGUARD / CUSTOM */
    char    dns_custom[16]; /* custom DNS IP string e.g. "8.8.8.8" */
    uint8_t ad_filter;      /* 0 = off, 1 = local blocklist        */
} bridge_config_t;

/* Global config instance — defined in config.c */
extern bridge_config_t g_cfg;

/* Load from NVS (falls back to defaults on first boot) */
void config_load(bridge_config_t *cfg);

/* Persist all fields to NVS */
void config_save(const bridge_config_t *cfg);
