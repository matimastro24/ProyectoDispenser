#include "http_server.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "local_db.h"
#include "nvs_flash.h"
#include "sdkconfig.h" // <- imprescindible
#include "state_machine.h"
#include <esp_log.h>
#include <string.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la
// config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

EventGroupHandle_t s_wifi_event_group;
TimerHandle_t s_wifi_retry_timer;

#define WIFI_NAMESPACE "wifi_cfg"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

// AP fijo
#define AP_SSID "ConfigESP32"
#define AP_PASS "12345678"

// Si no hay nada en NVS, usamos estas por defecto
#ifndef WIFI_SSID
#define WIFI_SSID "ESP32"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "matias123"
#endif
#define MAX_RETRY 5

static char g_ssid[MAX_SSID_LEN + 1] = {0};
static char g_pass[MAX_PASS_LEN + 1] = {0};
static bool g_have_credentials = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "HTTP_SERVER";

static const char *HTML_FORM =
	"<!DOCTYPE html>"
	"<html><head><meta charset='UTF-8'><title>Config WiFi</title></head>"
	"<body>"
	"<h2>Configuración WiFi STA</h2>"
	"<form action=\"/save\" method=\"GET\">"
	"SSID: <input type=\"text\" name=\"ssid\" value=\"%s\"><br><br>"
	"PASS: <input type=\"password\" name=\"pass\" value=\"%s\"><br><br>"
	"<input type=\"submit\" value=\"Guardar\">"
	"</form>"
	"<p>Conectado al AP: <b>ConfigESP32</b> (IP: 192.168.4.1)</p>"
	"</body></html>";

/**
 * @brief Cargar credenciales wifi guardadas en la nvs.
 */
static void load_wifi_credentials_from_nvs(void) {
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "No hay credenciales en NVS (%s)", esp_err_to_name(err));
		return;
	}

	size_t len_ssid = sizeof(g_ssid);
	size_t len_pass = sizeof(g_pass);

	err = nvs_get_str(nvs, "ssid", g_ssid, &len_ssid);
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "No se pudo leer ssid (%s)", esp_err_to_name(err));
		nvs_close(nvs);
		return;
	}

	err = nvs_get_str(nvs, "pass", g_pass, &len_pass);
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "No se pudo leer pass (%s)", esp_err_to_name(err));
		nvs_close(nvs);
		return;
	}

	nvs_close(nvs);
	g_have_credentials = true;
	ESP_LOGI(TAG, "Credenciales NVS: ssid='%s'", g_ssid);
}
/**
 * @brief Guardar credenciales wifi en la nvs.
 */
static void save_wifi_credentials_to_nvs(const char *ssid, const char *pass) {
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Error abriendo NVS (%s)", esp_err_to_name(err));
		return;
	}

	ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
	ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", pass));
	ESP_ERROR_CHECK(nvs_commit(nvs));

	nvs_close(nvs);

	strncpy(g_ssid, ssid, sizeof(g_ssid) - 1);
	strncpy(g_pass, pass, sizeof(g_pass) - 1);
	g_have_credentials = true;

	ESP_LOGI(TAG, "Credenciales guardadas en NVS");
}

/**
 * @brief get-handler, formulario para ingresar credenciales
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
	char resp[1024];
	const char *ssid = g_have_credentials ? g_ssid : "";
	const char *pass = g_have_credentials ? g_pass : "";
	snprintf(resp, sizeof(resp), HTML_FORM, ssid, pass);

	httpd_resp_set_type(req, "text/html");
	httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
	// httpd_resp_send(req, root_start, root_len);
	return ESP_OK;
}

/**
 * @brief get-handler, capturar credenciales del formulario
 */
static esp_err_t save_get_handler(httpd_req_t *req) {
	char ssid[MAX_SSID_LEN + 1] = {0};
	char pass[MAX_PASS_LEN + 1] = {0};

	int qlen = httpd_req_get_url_query_len(req) + 1;
	if (qlen > 1) {
		char *buf = malloc(qlen);
		if (buf) {
			if (httpd_req_get_url_query_str(req, buf, qlen) == ESP_OK) {
				httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
				httpd_query_key_value(buf, "pass", pass, sizeof(pass));
			}
			free(buf);
		}
	}

	ESP_LOGI(TAG, "Nuevas credenciales: ssid='%s', pass='%s'", ssid, pass);
	if (strlen(ssid) == 0) {
		httpd_resp_sendstr(req, "SSID vacío, volver atrás");
		return ESP_OK;
	}

	// Guardar en NVS
	save_wifi_credentials_to_nvs(ssid, pass);

	// Reconfigurar STA
	wifi_config_t sta_config = {0};
	strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
	strncpy((char *)sta_config.sta.password, pass,
			sizeof(sta_config.sta.password) - 1);
	sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
	sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
	sta_config.sta.pmf_cfg.capable = true;
	sta_config.sta.pmf_cfg.required = false;

	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
	ESP_ERROR_CHECK(esp_wifi_disconnect());
	ESP_ERROR_CHECK(esp_wifi_connect());

	const char *resp = "<html><body><h3>Guardado!</h3>"
					   "<p>El ESP32 intentar&aacute; conectarse a la nueva red "
					   "luego del reinicio.</p>"
					   "<a href=\"/\">Volver</a>"
					   "</body></html>";

	httpd_resp_set_type(req, "text/html");
	httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
	vTaskDelay(pdMS_TO_TICKS(1000));
	esp_restart();
	return ESP_OK;
}

/**
 * @brief Iniciar servidor http.
 */
void http_server_start(void) {
	httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
	httpd_cfg.server_port = 80;

	httpd_handle_t server = NULL;

	if (httpd_start(&server, &httpd_cfg) == ESP_OK) {

		httpd_uri_t uri_root = {.uri = "/",
								.method = HTTP_GET,
								.handler = root_get_handler,
								.user_ctx = NULL};
		httpd_register_uri_handler(server, &uri_root);

		httpd_uri_t uri_save = {.uri = "/save",
								.method = HTTP_GET,
								.handler = save_get_handler,
								.user_ctx = NULL};
		httpd_register_uri_handler(server, &uri_save);

		ESP_LOGI(TAG, "Servidor HTTP iniciado");
	} else {
		ESP_LOGE(TAG, "No se pudo iniciar HTTP server");
	}
}

/**
 * @brief Timer para reintentar conexion wifi.
 */
static void wifi_retry_timer_callback(TimerHandle_t xTimer) {
	ESP_LOGI(TAG,
			 "Temporizador de reintento: llamando a esp_wifi_connect()...");
	esp_wifi_connect();
}

void wifi_event_handler(void *arg, esp_event_base_t event_base,
						int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT &&
			   event_id == WIFI_EVENT_STA_DISCONNECTED) {
		xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGW(TAG, "Red Wi-Fi perdida. Reintentando conectar...");
		xTimerStart(s_wifi_retry_timer, 0);
		//esp_wifi_connect();
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

/**
 * @brief Iniciar wifi en modo AP/STA.
 */
void wifi_init_apsta(void) {
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	// Ambas interfaces
	esp_netif_create_default_wifi_ap();
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

	// 1) Cargar credenciales NVS si existen
	load_wifi_credentials_from_nvs();

	// 2) Config AP
	wifi_config_t ap_config = {
		.ap =
			{
				.ssid = AP_SSID,
				.ssid_len = 0,
				.password = AP_PASS,
				.max_connection = 4,
				.authmode = WIFI_AUTH_WPA_WPA2_PSK,
			},
	};
	if (strlen((char *)ap_config.ap.password) == 0) {
		ap_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	// 3) Config STA
	wifi_config_t sta_config = {0};
	sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
	sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
	sta_config.sta.pmf_cfg.capable = true;
	sta_config.sta.pmf_cfg.required = false;

	if (g_have_credentials) {
		strncpy((char *)sta_config.sta.ssid, g_ssid,
				sizeof(sta_config.sta.ssid) - 1);
		strncpy((char *)sta_config.sta.password, g_pass,
				sizeof(sta_config.sta.password) - 1);
		ESP_LOGI(TAG, "Usando credenciales NVS para STA");
	} else {
		strncpy((char *)sta_config.sta.ssid, WIFI_SSID,
				sizeof(sta_config.sta.ssid) - 1);
		strncpy((char *)sta_config.sta.password, WIFI_PASS,
				sizeof(sta_config.sta.password) - 1);
		ESP_LOGI(TAG, "Usando credenciales por defecto para STA");
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "AP levantado: SSID:%s  PASS:%s", AP_SSID, AP_PASS);
	ESP_LOGI(TAG, "Esperando conexión STA...");

	s_wifi_retry_timer =
		xTimerCreate("wifi_retry",		   // Nombre (para debug)
					 pdMS_TO_TICKS(10000), // 10000 ms
					 pdFALSE,			   // No se auto-recarga (es "one-shot")
					 NULL,				   // Sin ID de timer
					 wifi_retry_timer_callback // La función que llamará
		);
}

bool wifi_is_connected(void) {
	EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
	return (bits & WIFI_CONNECTED_BIT);
}

//=======================================================
// Codigo para la implementaicon de la base de datos local
//=======================================================

#define TAG "HTTP_CLIENT"
#define URL_SCRIPT                                                             \
	"https://script.google.com/macros/s/"                                      \
	"AKfycbwWRQocdLq13cf1czfCb8BNM0pRrFgPVpoc2TCTqiJtHO3_astKrJcsP_DZ13osDn_"  \
	"vVg/exec"
#define URL_SCRIPT_VERSION                                                     \
	"https://script.google.com/macros/s/"                                      \
	"AKfycbwWRQocdLq13cf1czfCb8BNM0pRrFgPVpoc2TCTqiJtHO3_astKrJcsP_DZ13osDn_"  \
	"vVg/exec?cmd=version"

/*
esp_crt_bundle_attach debe estar activaod en la copnfiguracion!!!!
Component config -> mbedTLS -> Certificate Bundle
*/

// Buffer para ir acumulando la respuesta (el JSON)
#define MAX_HTTP_OUTPUT_BUFFER 2048
char *response_buffer = NULL;
int response_len = 0;

static FILE *f_temp = NULL;

// Manejador de eventos HTTP (Google manda la data en pedacitos "chunks")
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
	switch (evt->event_id) {
	case HTTP_EVENT_ERROR:
		ESP_LOGE(TAG, "Error HTTP");
		break;

	case HTTP_EVENT_ON_CONNECTED:
		ESP_LOGI(TAG, "Conectado al servidor");
		break;

	case HTTP_EVENT_ON_HEADER:
		// Aquí podrías verificar tipos de contenido si quisieras
		break;
	case HTTP_EVENT_ON_DATA:
		// 1. OBTENER EL CÓDIGO DE ESTADO ACTUAL
		int status_code = esp_http_client_get_status_code(evt->client);

		// 2. FILTRO DE SEGURIDAD
		// Google Apps Script siempre redirige (302) antes de dar la data (200).
		// Solo escribimos si es el destino final (200).
		if (status_code == 200) {

			if (f_temp != NULL) {
				fwrite(evt->data, 1, evt->data_len, f_temp);
			}

		} else {
			// Si entra aquí es el HTML del "Moved Temporarily". Lo ignoramos.
			ESP_LOGD(TAG, "Ignorando datos de redireccion (Status: %d)",
					 status_code);
		}
		break;
	case HTTP_EVENT_ON_FINISH:
		ESP_LOGI(TAG, "Finalizo la conexion HTTP");
		break;

	case HTTP_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "Desconectado");
		break;
	default:
		break;
	}
	return ESP_OK;
}

/*
descarga la base de datos de google sheet hacia la flash
*/
esp_err_t descargar_base_datos() {
	// 1. Abrir archivo temporal para escritura
	f_temp = fopen("/littlefs/temp.json", "w");
	if (f_temp == NULL) {
		ESP_LOGE(TAG, "No se pudo crear el archivo temporal en LittleFS");
		return ESP_FAIL;
	}

	esp_http_client_config_t config = {
		.url = URL_SCRIPT,
		.event_handler = _http_event_handler,
		.method = HTTP_METHOD_GET,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.timeout_ms = 15000, // Demos tiempo si la red es lenta
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);

	ESP_LOGI(TAG, "Iniciando descarga...");
	esp_err_t err = esp_http_client_perform(client);

	// 2. Cerrar el archivo SIEMPRE, pase lo que pase
	fclose(f_temp);
	f_temp = NULL; // Resetear puntero por seguridad

	if (err == ESP_OK) {
		int status_code = esp_http_client_get_status_code(client);
		ESP_LOGI(TAG, "Estado HTTP: %d", status_code);

		// 3. Validar éxito (Código 200)
		if (status_code == 200) {
			ESP_LOGI(TAG, "Descarga exitosa. Actualizando base de datos...");

			// Borrar la DB vieja
			unlink("/littlefs/usuarios.json");

			// Renombrar temp a oficial
			if (rename("/littlefs/temp.json", "/littlefs/usuarios.json") == 0) {
				ESP_LOGI(TAG, "Base de datos actualizada correctamente.");
			} else {
				ESP_LOGE(TAG, "Error al renombrar el archivo temporal.");
			}
		} else {
			ESP_LOGE(TAG,
					 "Servidor respondio con error (no 200). Borrando temp.");
			unlink("/littlefs/temp.json");
		}
	} else {
		ESP_LOGE(TAG, "Fallo la peticion HTTP: %s", esp_err_to_name(err));
		unlink("/littlefs/temp.json");
	}

	return esp_http_client_cleanup(client);
}

// Leer versión guardada (Devuelve 0 si nunca se guardó nada)
uint16_t leer_version_local(void) {
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
	uint32_t version = 0; // Default por si es la primera vez

	if (err == ESP_OK) {
		nvs_get_u32(my_handle, "db_ver", &version);
		nvs_close(my_handle);
	}
	return version;
}
// Guardar nueva versión
esp_err_t guardar_version_local(uint32_t nueva_version) {
	nvs_handle_t my_handle;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
	if (err == ESP_OK) {
		nvs_set_u32(my_handle, "db_ver", nueva_version);
		nvs_commit(my_handle); // ¡Importante para persistir!
		nvs_close(my_handle);
	}
	return err;
}

typedef struct {
	char *buffer;
	int max_len;
	int index;
} respuesta_string_t;

// Este handler guarda lo que recibe en la estructura de RAM que le pasemos
esp_err_t _handler_version(esp_http_client_event_t *evt) {
	switch (evt->event_id) {
	case HTTP_EVENT_ON_DATA:
		// 1. OBTENER EL CÓDIGO DE ESTADO
		int status = esp_http_client_get_status_code(evt->client);

		// 2. FILTRAR: Solo nos interesa si es 200 (OK)
		// Si es 302, es el HTML de redirección y lo ignoramos.
		if (status == 200) {

			if (evt->user_data != NULL) {
				respuesta_string_t *resp = (respuesta_string_t *)evt->user_data;

				// Solo copiamos si hay espacio
				if (resp->index < resp->max_len - 1) {
					int bytes_to_copy = evt->data_len;

					if (resp->index + bytes_to_copy >= resp->max_len) {
						bytes_to_copy = resp->max_len - 1 - resp->index;
					}

					memcpy(resp->buffer + resp->index, evt->data,
						   bytes_to_copy);
					resp->index += bytes_to_copy;
					resp->buffer[resp->index] = '\0';
				}
			}
		}
		// Si es 302, no hacemos nada, dejamos el buffer limpio (index en 0)
		break;

	default:
		break;
	}
	return ESP_OK;
}

uint16_t obtener_version_nube(void) {
	char local_buffer[32] = {
		0}; // Buffer pequeño, solo esperamos un número (ej: "15")

	// Preparamos la estructura de contexto
	respuesta_string_t response_data = {
		.buffer = local_buffer, .max_len = sizeof(local_buffer), .index = 0};

	esp_http_client_config_t config = {
		.url = URL_SCRIPT_VERSION,
		.event_handler = _handler_version,
		.user_data = &response_data,
		.method = HTTP_METHOD_GET,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.timeout_ms = 5000,
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);

	ESP_LOGI("VERSION", "Consultando versión a Google Sheets...");
	esp_err_t err = esp_http_client_perform(client);

	int version_retornada = 0; // 0 indica error

	if (err == ESP_OK) {
		int status = esp_http_client_get_status_code(client);
		if (status == 200) {
			ESP_LOGI("VERSION", "Texto recibido crudo: '%s'", local_buffer);
			// Convertimos el texto "15" a entero 15
			version_retornada = atoi(local_buffer);

			ESP_LOGI("VERSION", "Versión en nube: %d", version_retornada);
		} else {
			ESP_LOGW("VERSION", "Error HTTP: %d", status);
		}
	} else {
		ESP_LOGE("VERSION", "Fallo conexión: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return version_retornada;
}
