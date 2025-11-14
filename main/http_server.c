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
#include "esp_http_server.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la
// config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif


EventGroupHandle_t s_wifi_event_group;

#define WIFI_NAMESPACE   "wifi_cfg"
#define MAX_SSID_LEN     32
#define MAX_PASS_LEN     64

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

#define GSCRIPT_BASE                                                           \
	"https://script.google.com/macros/s/"                                      \
	"AKfycbyLY1mi1zXoB3DVN1218tuNaLsfvXk8MdQWMtuxRMkNIshLWVJzXq0T6pVDu5d_"     \
	"V3z9pQ/exec"


static int s_retry_num = 0;
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

static void load_wifi_credentials_from_nvs(void)
{
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

static void save_wifi_credentials_to_nvs(const char *ssid, const char *pass)
{
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

    strncpy(g_ssid, ssid, sizeof(g_ssid)-1);
    strncpy(g_pass, pass, sizeof(g_pass)-1);
    g_have_credentials = true;

    ESP_LOGI(TAG, "Credenciales guardadas en NVS");
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char resp[1024];
    const char *ssid = g_have_credentials ? g_ssid : "";
    const char *pass = g_have_credentials ? g_pass : "";

    snprintf(resp, sizeof(resp), HTML_FORM, ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_get_handler(httpd_req_t *req)
{
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
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid)-1);
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password)-1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_connect());

    const char *resp =
        "<html><body><h3>Guardado!</h3>"
        "<p>El ESP32 intentar&aacute; conectarse a la nueva red.</p>"
        "<a href=\"/\">Volver</a>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Prototipos de los handlers que ya definiste antes
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t save_get_handler(httpd_req_t *req);

void http_server_start(void)
{
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.server_port = 80;  // si querés cambiar el puerto

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &httpd_cfg) == ESP_OK) {

        httpd_uri_t uri_root = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_save = {
            .uri      = "/save",
            .method   = HTTP_GET,
            .handler  = save_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_save);

        ESP_LOGI(TAG, "Servidor HTTP iniciado");
    } else {
        ESP_LOGE(TAG, "No se pudo iniciar HTTP server");
    }
}



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

void wifi_init_apsta(void)
{
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
        .ap = {
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
    wifi_config_t sta_config = { 0 };
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    if (g_have_credentials) {
        strncpy((char *)sta_config.sta.ssid, g_ssid, sizeof(sta_config.sta.ssid)-1);
        strncpy((char *)sta_config.sta.password, g_pass, sizeof(sta_config.sta.password)-1);
        ESP_LOGI(TAG, "Usando credenciales NVS para STA");
    } else {
        strncpy((char *)sta_config.sta.ssid, WIFI_SSID, sizeof(sta_config.sta.ssid)-1);
        strncpy((char *)sta_config.sta.password, WIFI_PASS, sizeof(sta_config.sta.password)-1);
        ESP_LOGI(TAG, "Usando credenciales por defecto para STA");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP levantado: SSID:%s  PASS:%s", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "Esperando conexión STA...");

    // OJO: NO llamamos esp_wifi_connect() acá si ya lo hace el handler en WIFI_EVENT_STA_START

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
        pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA conectada");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "STA: No se pudo conectar al WiFi");
    } else {
        ESP_LOGE(TAG, "STA: Timeout esperando WiFi");
    }
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

bool wifi_is_connected(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
