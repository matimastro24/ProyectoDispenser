idf_component_register(SRCS "time_sync.c" "local_db.c" "state_machine.c" "rfid.c" "main.c" 
						    "http_server.c"
						    "keypad.c"
						    "buzzer.c"
                            "lcd_driver.c"
                            "local_db"
                            "time_sync"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event esp_http_client nvs_flash cjson esp_http_client esp_http_server spi_flash

                    )
