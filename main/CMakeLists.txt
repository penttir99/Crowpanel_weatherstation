FILE(GLOB_RECURSE main ${CMAKE_SOURCE_DIR}/main/*.c)
idf_component_register(SRCS ${main}
                        REQUIRES 
                            bsp_illuminate
                            esp_wifi
                            esp_event
                            esp_netif
                            nvs_flash
                            esp_http_client
                            json
			    bsp_display
                            bsp_i2c
                            espressif__esp_lcd_touch_gt911
                            mbedtls
                            esp_driver_jpeg
                            lwip)
                           