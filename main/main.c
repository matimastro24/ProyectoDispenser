#include <esp_log.h>
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"
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

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
// forward declaration: evita incluir el header y alcanza para asignarlo en la config
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

static const char *TAG = "rc522-basic-example";

#define RC522_SPI_BUS_GPIO_MISO    (19)
#define RC522_SPI_BUS_GPIO_MOSI    (23)
#define RC522_SPI_BUS_GPIO_SCLK    (18)
#define RC522_SPI_SCANNER_GPIO_SDA (5)
#define RC522_SCANNER_GPIO_RST     (-1) // soft-reset

static rc522_spi_config_t driver_config = {
    .host_id = SPI3_HOST,
    .bus_config = &(spi_bus_config_t){
        .miso_io_num = RC522_SPI_BUS_GPIO_MISO,
        .mosi_io_num = RC522_SPI_BUS_GPIO_MOSI,
        .sclk_io_num = RC522_SPI_BUS_GPIO_SCLK,
    },
    .dev_config = {
        .spics_io_num = RC522_SPI_SCANNER_GPIO_SDA,
    },
    .rst_io_num = RC522_SCANNER_GPIO_RST,
};

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX];
    	rc522_picc_uid_to_str(&picc->uid, uid_str, sizeof(uid_str));
    	ESP_LOGI(TAG, "RFID UID: %s", uid_str);
    	http_get_uid_async(uid_str);

    }
    else if (picc->state == RC522_PICC_STATE_IDLE && event->old_state >= RC522_PICC_STATE_ACTIVE) {
        ESP_LOGI(TAG, "Card has been removed");
    }
}


// Mapeo físico
static const gpio_num_t ROWS[4] = { GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_27 };  // ENTRADAS con pull-up
static const gpio_num_t COLS[4] = { GPIO_NUM_26, GPIO_NUM_25, GPIO_NUM_33, GPIO_NUM_32 };  // SALIDAS

// Mapa de teclas (ajustalo a tu preferencia)
static const char KEYMAP[4][4] = {
/*            C0   C1   C2   C3  */
    /* R0 */ {'1', '2', '3', 'A'},
    /* R1 */ {'4', '5', '6', 'B'},
    /* R2 */ {'7', '8', '9', 'C'},
    /* R3 */ {'*', '0', '#', 'D'},
};

// --- Utilidades ---
static inline void cols_high_impedance_startup_safe(void) {
    // Opcional: durante init, dejá las columnas en entrada (hi-Z) por un instante
    // para minimizar cualquier efecto al arranque (especialmente GPIO4).
    for (int c = 0; c < 4; c++) {
        gpio_reset_pin(COLS[c]);
        gpio_set_direction(COLS[c], GPIO_MODE_INPUT);
    }
}

static inline void set_all_cols_high(void) {
    for (int c = 0; c < 4; c++) {
        gpio_set_level(COLS[c], 1);
    }
}

static inline void drive_only_col_low(int col) {
    for (int c = 0; c < 4; c++) {
        gpio_set_level(COLS[c], (c == col) ? 0 : 1);
    }
}

// --- API ---
void keypad_init(void) {
    // 1) Filas como entradas con pull-up
    gpio_config_t in_conf = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    for (int r = 0; r < 4; r++) {
        in_conf.pin_bit_mask |= (1ULL << ROWS[r]);
    }
    ESP_ERROR_CHECK(gpio_config(&in_conf));

    // 2) Dejar columnas en alta impedancia un instante (opcional, prudente por GPIO4)
    cols_high_impedance_startup_safe();
    vTaskDelay(pdMS_TO_TICKS(5));

    // 3) Columnas como salidas, nivel alto por defecto
    gpio_config_t out_conf = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    for (int c = 0; c < 4; c++) {
        out_conf.pin_bit_mask |= (1ULL << COLS[c]);
    }
    ESP_ERROR_CHECK(gpio_config(&out_conf));
    set_all_cols_high();

    ESP_LOGI(TAG, "Keypad 4x4 inicializado");
}

/**
 * Escanea el keypad una vez.
 * Si encuentra una tecla estable (con debounce), escribe en *out_key y retorna true.
 * Devuelve false si no hay tecla.
 */
bool keypad_scan_once(char *out_key, TickType_t debounce_ms) {
    for (int c = 0; c < 4; c++) {
        drive_only_col_low(c);
        // Pequeño settle time por la matriz
        vTaskDelay(pdMS_TO_TICKS(10));

        // Leer filas: activas en LOW
        for (int r = 0; r < 4; r++) {
            int val = gpio_get_level(ROWS[r]);
            if (val == 0) {
                // Debounce: esperar y confirmar
                vTaskDelay(pdMS_TO_TICKS(debounce_ms));
                if (gpio_get_level(ROWS[r]) == 0) {
                    // Esperar liberación antes de salir (key release)
                    *out_key = KEYMAP[r][c];

                    // Mantener la misma columna en LOW hasta que se suelte
                    while (gpio_get_level(ROWS[r]) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                    set_all_cols_high();
                    return true;
                }
            }
        }
    }
    set_all_cols_high();
    return false;
}


void app_main()
{
    rc522_spi_create(&driver_config, &driver);
    rc522_driver_install(driver);

    rc522_config_t scanner_config = {
        .driver = driver,
    };

    rc522_create(&scanner_config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
    rc522_start(scanner);
    
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    
	
	keypad_init();
    char key;
    while (1) {
        if (keypad_scan_once(&key, 10)) {
            ESP_LOGI(TAG, "Tecla: %c", key);
        }
        vTaskDelay(pdMS_TO_TICKS(5)); // periodo de escaneo
    }

	// Enviar DNI + PIN
	//http_get_dni_pin_async("43425034", "2407");
	
    
}
