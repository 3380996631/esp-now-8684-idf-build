idf_component_register(SRCS "main.cpp"
                    REQUIRES esp_driver_uart bridge_proto espnow_link uart_link
                    INCLUDE_DIRS ".")
