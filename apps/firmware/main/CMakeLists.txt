idf_component_register(SRCS "momento_main.c"
                            "sd_card.c"
                            "mic.c"
                            "avi_writer.c"
                            "wav_writer.c"
                            "wifi_sync.c"
                       PRIV_REQUIRES esp_driver_gpio
                                     esp_driver_i2c
                                     fatfs
                                     sdmmc
                                     esp_driver_sdspi
                                     esp_driver_spi
                                     esp_driver_i2s
                                     esp_timer
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     esp_http_server
                                     nvs_flash
                       INCLUDE_DIRS ".")
