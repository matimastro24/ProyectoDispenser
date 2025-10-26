#include <stddef.h>
#include "esp_err.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <esp_log.h>
#include "buzzer.h"

const char *TAG = "keypad";


// Mapeo físico
const gpio_num_t ROWS[4] = { GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_27 };  // ENTRADAS con pull-up
const gpio_num_t COLS[4] = { GPIO_NUM_26, GPIO_NUM_25, GPIO_NUM_33, GPIO_NUM_32 };  // SALIDAS

// Mapa de teclas (ajustalo a tu preferencia)
const char KEYMAP[4][4] = {
/*            C0   C1   C2   C3  */
    /* R0 */ {'1', '2', '3', 'A'},
    /* R1 */ {'4', '5', '6', 'B'},
    /* R2 */ {'7', '8', '9', 'C'},
    /* R3 */ {'*', '0', '#', 'D'},
};


// --- Utilidades ---
void cols_high_impedance_startup_safe(void) {
    // Opcional: durante init, dejá las columnas en entrada (hi-Z) por un instante
    // para minimizar cualquier efecto al arranque (especialmente GPIO4).
    for (int c = 0; c < 4; c++) {
        gpio_reset_pin(COLS[c]);
        gpio_set_direction(COLS[c], GPIO_MODE_INPUT);
    }
}

void set_all_cols_high(void) {
    for (int c = 0; c < 4; c++) {
        gpio_set_level(COLS[c], 1);
    }
}

void drive_only_col_low(int col) {
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

    // 2) Dejar columnas en alta impedancia un instante (opcional, prudente por GPIO12)
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

					setBuzzer(true);

                    // Mantener la misma columna en LOW hasta que se suelte
                    while (gpio_get_level(ROWS[r]) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                    set_all_cols_high();

                    setBuzzer(false);

                    
                    return true;
                }
            }
        }
    }
    set_all_cols_high();
    return false;
}