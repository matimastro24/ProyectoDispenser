#include <esp_log.h>
#include "driver/gpio.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "sdkconfig.h"
#include "http_server.h"                  // <- imprescindible
#include "keypad.h" 
#include "buzzer.h"
#include "lcd_driver.h"
#include "rfid.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

static const char *TAG = "MAIN";



static rc522_driver_handle_t driver;
static rc522_handle_t scanner;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t lcd_dev;
static lcd_t lcd;

void app_main()
{

    rfid_init(&driver, &scanner);
    rc522_start(scanner);
    
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    
	buzzer();
	keypad_init();
	

    i2c_init(&i2c_bus, &lcd_dev);
    lcd_init(&lcd, lcd_dev);


    //iniciar lcd iniciado
    lcd_clear(&lcd);
    lcd_set_cursor(&lcd, 0, 0);
    lcd_write_string(&lcd,"fila:0, col:0");
    lcd_set_cursor(&lcd, 1, 1);
    lcd_write_string(&lcd,"fila:1, col:1");
    lcd_set_cursor(&lcd, 2, 2);
    lcd_write_string(&lcd,"fila:2, col:2");
    lcd_set_cursor(&lcd, 3, 3);
    lcd_write_string(&lcd,"fila:3, col:3");

	
    char key;
    while (1) {
        if (keypad_scan_once(&key, 10)) {
            ESP_LOGI(TAG, "Tecla: %c", key);
        }
        vTaskDelay(pdMS_TO_TICKS(5)); // periodo de escaneo
    }
}

