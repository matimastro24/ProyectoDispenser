#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"   

 void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
                          
void wifi_init_sta(void);
esp_err_t http_event_handler(esp_http_client_event_t *evt);
void http_get_task(void *pv);
void http_get_uid_async(const char *uid_hex_or_num);
void http_get_dni_pin_async(const char *dni, const char *pin);
void procesar_json(const char *json_str);