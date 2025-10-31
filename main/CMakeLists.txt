idf_component_register(SRCS "usbzone.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES usb fatfs esp_driver_gpio esp_timer esp_wifi nvs_flash esp_http_server mbedtls
                    )


