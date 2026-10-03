idf_component_register(SRCS "espnow_link.c"
                    REQUIRES esp_wifi nvs_flash esp_netif esp_event
                    INCLUDE_DIRS "include")
