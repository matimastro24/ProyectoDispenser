idf_component_register(SRCS "main.c" 
						    "http_server.c"
						    "keypad.c"
						    "buzzer.c"
                            "lcd_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event esp_http_client nvs_flash cjson
                    )
