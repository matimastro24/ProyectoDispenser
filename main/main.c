#include "buzzer.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http_server.h" // <- imprescindible
#include "keypad.h"
#include "lcd_driver.h"
#include "local_db.h"
#include "nvs_flash.h"
#include "rfid.h"
#include "sdkconfig.h"
#include "state_machine.h"
#include <esp_log.h>
#include <string.h>
#include "freertos/semphr.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la
// config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

static const char *TAG = "MAIN";

void app_main(void) {
	// Inicializar NVS (Obligatorio para WiFi)
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
		ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	
	state_machine_init();

	

	char key;

	while (1) {
		// Actualizar máquina de estados (timeouts, etc)
		state_machine_update();
		// Escanear teclado
		if (keypad_scan_once(&key, 10)) {
			ESP_LOGI(TAG, "Tecla presionada: %c", key);

			// Actulizar maquina de estos al detectarse una tecla.
			state_machine_key_pressed(key);
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}
