#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"   
#include "freertos/event_groups.h"

 void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
                          
void wifi_init_apsta(void);
esp_err_t http_event_handler(esp_http_client_event_t *evt);
void http_get_task(void *pv);
bool wifi_is_connected(void);
void http_server_start(void); // arranca el servidor web		
extern EventGroupHandle_t s_wifi_event_group;
esp_err_t descargar_base_datos(void);
/**
 * @brief Obtener version de la DB local.
 * @return [int] Version. En caso de que no haya nada devuelve 0.
 * Y si falla abriendo la NVS tambien devuelve 0.
 */
uint16_t leer_version_local(void);

/**
 * @brief Guarda en numero de la version local de la db en la NVS.
 * @return - ESP-OK se guardo correctamente.
 * @return - ESP-FAIL error.
 */
esp_err_t guardar_version_local(uint32_t nueva_version);

/**
 * @brief Obtener version de la DB de la nube.
 * @return [int] Version. En caso de error devuelve 0.
 */
uint16_t obtener_version_nube(void);



