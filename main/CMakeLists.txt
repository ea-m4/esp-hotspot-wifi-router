idf_component_register(SRCS "wifi-hotspot_bridge.c"
                              "config.c"
                              "wifi_bridge.c"
                              "web_config.c"
                              "dns_filter.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi nvs_flash esp_http_server)
