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
#include "nvs_flash.h"
#include "rfid.h"
#include "sdkconfig.h"
#include "state_machine.h"
#include <esp_log.h>
#include <string.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la
// config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

static const char *TAG = "MAIN";

// Handles globales
static rc522_driver_handle_t driver;
static rc522_handle_t scanner;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t lcd_dev;
static lcd_t lcd;

void app_main(void) {
	// Inicialización de componentes
	ESP_ERROR_CHECK(nvs_flash_init());
	i2c_init(&i2c_bus, &lcd_dev);
	lcd_init(&lcd, lcd_dev);
	lcd_clear(&lcd);
	lcd_write_string(&lcd, "Iniciando Sistema...");

	wifi_init_apsta();
	http_server_start();

	buzzer();
	keypad_init();
	// Inicializar RFID y registrar callback
	rfid_init(&driver, &scanner);
	// Inicializar máquina de estados
	state_machine_init(&lcd, &scanner);
	ESP_LOGI(TAG, "Sistema inicializado correctamente");

	// Loop principal
	char key;
	while (1) {
		// Actualizar máquina de estados (timeouts, etc)
		if (wifi_is_connected()) {
			state_machine_update_w_wifi();
		} else {
			state_machine_update_no_wifi();
		}
		// Escanear teclado
		if (keypad_scan_once(&key, 10)) {
			ESP_LOGI(TAG, "Tecla presionada: %c", key);

			// Actulizar maquina de estos al detectarse una tecla.
			state_machine_key_pressed(key);
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}
