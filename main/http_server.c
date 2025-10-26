#include "http_server.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h" // <- imprescindible
#include <esp_log.h>
#include <string.h>
#include "state_machine.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la
// config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

#define WIFI_SSID "WIFICLARO"
#define WIFI_PASS "Claro6296"
#define MAX_RETRY 5

#define GSCRIPT_BASE                                                           \
	"https://script.google.com/macros/s/"                                      \
	"AKfycbyLY1mi1zXoB3DVN1218tuNaLsfvXk8MdQWMtuxRMkNIshLWVJzXq0T6pVDu5d_"     \
	"V3z9pQ/exec"

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "HTTP_SERVER";

void wifi_event_handler(void *arg, esp_event_base_t event_base,
						int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT &&
			   event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < MAX_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGW(TAG, "reintentando conexión al WiFi (%d/%d)", s_retry_num,
					 MAX_RETRY);
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}
void wifi_init_sta(void) {
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
		&instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
		&instance_got_ip));

	wifi_config_t wifi_config = {
		.sta =
			{
				.ssid = WIFI_SSID,
				.password = WIFI_PASS,
				.threshold.authmode = WIFI_AUTH_WPA2_PSK,
				.sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
			},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "Conectando a SSID:%s ...", WIFI_SSID);

	EventBits_t bits = xEventGroupWaitBits(
		s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
		pdFALSE, pdMS_TO_TICKS(20000));
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "Conectado al WiFi");
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGE(TAG, "No se pudo conectar al WiFi");
	} else {
		ESP_LOGE(TAG, "Timeout esperando WiFi");
	}

	// xTaskCreate(&http_get_task, "http_get_task", 8192, NULL, 5, NULL);
}

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
	static char *accum = NULL;
	static int accum_len = 0;

	switch (evt->event_id) {
	case HTTP_EVENT_ON_DATA:
		if (evt->data_len > 0) {
			char *newbuf = realloc(accum, accum_len + evt->data_len + 1);
			if (newbuf) {
				accum = newbuf;
				memcpy(accum + accum_len, evt->data, evt->data_len);
				accum_len += evt->data_len;
				accum[accum_len] = 0;
			}
		}
		break;
	case HTTP_EVENT_ON_FINISH:
		if (accum) {
			ESP_LOGI(TAG, "Respuesta (%d bytes):\n%.*s", accum_len, accum_len,
					 accum);
			procesar_json(accum);
			free(accum);
			accum = NULL;
			accum_len = 0;
		}
		break;
	case HTTP_EVENT_DISCONNECTED:
		// limpiar si quedó algo
		if (accum) {
			free(accum);
			accum = NULL;
			accum_len = 0;
		}
		break;
	default:
		break;
	}
	return ESP_OK;
}

// pv debe ser un char* malloc'd (ej: strdup), con la query: "uid=123" o
// "dni=...&pin=..."
void http_get_task(void *pv) {
	char *query = (char *)pv; // lo liberamos al final
	char url[512];

	// arma la URL final: BASE + ? + query   (si GSCRIPT_BASE ya trajera '?',
	// usaría '&')
	const bool base_has_q = (strchr(GSCRIPT_BASE, '?') != NULL);
	snprintf(url, sizeof(url), "%s%s%s", GSCRIPT_BASE, base_has_q ? "&" : "?",
			 query ? query : "");
	// ESP_LOGI(TAG, "URL: %s", url);
	esp_http_client_config_t config = {
		.url = url,
		.method = HTTP_METHOD_GET,
		.event_handler = http_event_handler,
		.timeout_ms = 10000,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
		.crt_bundle_attach = esp_crt_bundle_attach,
#endif
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client) {
		ESP_LOGE(TAG, "No se pudo crear http client");

		// para mquina de estados
		//set_validation_result(false, "", "", 0);
        state_machine_validation_result(false, "","",0);
		if (query)
			free(query);
		vTaskDelete(NULL);
		return;
	}

	esp_err_t err = esp_http_client_perform(client);
	if (err == ESP_OK) {
		int status = esp_http_client_get_status_code(client);
		int cl = esp_http_client_get_content_length(client);
		ESP_LOGI(TAG, "GET OK status=%d content_length=%d", status, cl);
	} else {
		ESP_LOGE(TAG, "GET error: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	if (query)
		free(query); // liberamos lo que mandaste con strdup()
	vTaskDelete(NULL);
}

// Llama a la tarea pasando "uid=1234567"
void http_get_uid_async(const char *uid_hex_or_num) {
	char tmp[64];
	snprintf(tmp, sizeof(tmp), "uid=%s", uid_hex_or_num);
	xTaskCreate(&http_get_task, "http_get_task", 8192, strdup(tmp), 5, NULL);
}

// Llama a la tarea pasando "dni=...&pin=..."
void http_get_dni_pin_async(const char *dni, const char *pin) {
	char tmp[96];
	snprintf(tmp, sizeof(tmp), "dni=%s&pin=%s", dni, pin);
	xTaskCreate(&http_get_task, "http_get_task", 8192, strdup(tmp), 5, NULL);
}

void procesar_json(const char *json_str) {
	// Parsear el texto recibido
	cJSON *root = cJSON_Parse(json_str);
	if (root == NULL) {
		ESP_LOGE("JSON", "Error al parsear JSON");

		// maquina de estados
		//set_validation_result(false, "", "", 0);
        state_machine_validation_result(false, "","",0);
		return;
	}

	// Verificar si hay un error en la respuesta
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
	if (error && cJSON_IsString(error)) {
		ESP_LOGW(TAG, "Error del servidor: %s", error->valuestring);

		// maquina de estados
		// set_validation_result(false, "", "", 0);
        state_machine_validation_result(false, "", "", 0);
		cJSON_Delete(root);
		return;
	}

	// Leer los campos esperados
	const cJSON *nombre = cJSON_GetObjectItemCaseSensitive(root, "nombre");
	const cJSON *apellido = cJSON_GetObjectItemCaseSensitive(root, "apellido");
	const cJSON *extracciones =
		cJSON_GetObjectItemCaseSensitive(root, "extracciones");
        
	// Verificar y mostrar
	if (cJSON_IsString(nombre) && cJSON_IsString(apellido) &&
		cJSON_IsNumber(extracciones)) {
		ESP_LOGI("JSON", "Nombre: %s", nombre->valuestring);
		ESP_LOGI("JSON", "Apellido: %s", apellido->valuestring);
		ESP_LOGI("JSON", "Extracciones: %d", extracciones->valueint);

		// maquina de estados. Notificar al main con los datos del usuario
		/*
		set_validation_result(true,
							nombre->valuestring,
							apellido->valuestring,
							extracciones->valueint);
		*/
		state_machine_validation_result(true, nombre->valuestring,
											 apellido->valuestring,
											 extracciones->valueint);
	} else {
		ESP_LOGW("JSON", "Formato JSON inesperado");

		// maquina de estados
		/*
		set_validation_result(false, "", "", 0);
		*/
    state_machine_validation_result(false, "", "", 0);
	}

	// Liberar memoria
	cJSON_Delete(root);
}
