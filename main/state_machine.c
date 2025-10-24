#include "state_machine.h"
#include "buzzer.h"
#include "driver/gpio.h"
#include "http_server.h"
#include "rfid.h"
#include <esp_log.h>
#include <string.h>

#define RELE_GPIO GPIO_NUM_2
#define TIMEOUT_USER_INPUT_MS 30000	 // 30 segundos
#define DISPENSE_MAX_TIME_MS 10000	 // 60 segundos máximo de dispensación
#define VALIDATING_MAX_TIME_MS 30000 // 60 segundos máximo de dispensación

#define MENU_DNIPIN_SELECT_KEY 'A'

#define ENTER_DNI_ACCEPT_KEY 'A'
#define ENTER_DNI_CANCEL_KEY 'C'
#define ENTER_DNI_DELETE_KEY 'B'

#define ENTER_PIN_ACCEPT_KEY 'A'
#define ENTER_PIN_CANCEL_KEY 'C'
#define ENTER_PIN_DELETE_KEY 'B'

#define SHOW_USER_EXTRACT_KEY 'A'
#define SHOW_USER_CANCEL_KEY 'C'


#define DISPENSING_STOP_KEY 'D'


static const char *TAG = "STATE_MACHINE";

// Variables privadas del módulo
static lcd_t *lcd = NULL;
static rc522_handle_t *scanner = NULL;
static system_state_t current_state = STATE_MENU;

// Buffers de entrada
static char dni_buffer[10] = {0};
static char pin_buffer[5] = {0};
static uint8_t dni_index = 0;
static uint8_t pin_index = 0;

// Datos del usuario validado
static user_data_t user_data = {0};

//esta bansedara se pone en true cunado se ejecuta state_machine_validation_result
static bool validation_complete = false;

//Esta bandera se pone true si se puedo extraer nombre, apellido, extracciones
//durante la consulta http
//cualquier otro caso esta en false
static bool validation_success = false;

// Control de timeouts
static TickType_t state_start_time = 0;

// Prototipos de funciones privadas
static void show_menu(void);
static void show_enter_dni(void);
static void show_enter_pin(void);
static void show_validating(void);
static void show_user_info(void);
static void show_dispensing(void);
static void show_error(const char *msg);
static void reset_buffers(void);
static void activate_dispenser(void);
static void deactivate_dispenser(void);
static void change_state(system_state_t new_state);
static bool check_timeout(TickType_t timeout_ms);

// ============= FUNCIONES PÚBLICAS =============

void state_machine_init(lcd_t *lcd_ptr, rc522_handle_t *scanner_ptr) {
	lcd = lcd_ptr;
	scanner = scanner_ptr;

	// Inicializar GPIO del relé
	gpio_config_t io_conf = {.pin_bit_mask = (1ULL << RELE_GPIO),
							 .mode = GPIO_MODE_OUTPUT,
							 .pull_up_en = GPIO_PULLUP_DISABLE,
							 .pull_down_en = GPIO_PULLDOWN_DISABLE,
							 .intr_type = GPIO_INTR_DISABLE};
	gpio_config(&io_conf);
	gpio_set_level(RELE_GPIO, 0);

	change_state(STATE_MENU);
	ESP_LOGI(TAG, "Máquina de estados inicializada");
}

void state_machine_update(void) {
	// Verificar timeouts según el estado
	switch (current_state) {
	case STATE_ENTER_DNI:
	case STATE_ENTER_PIN:
	case STATE_SHOW_USER:
		if (check_timeout(TIMEOUT_USER_INPUT_MS)) {
			show_error("Timeout");
			vTaskDelay(pdMS_TO_TICKS(2000));
			change_state(STATE_MENU);
		}
		break;

	case STATE_VALIDATING:
		if (validation_complete) {
			if (validation_success) {
				change_state(STATE_SHOW_USER);
			} else {
				show_error("Usuario no valido");
				vTaskDelay(pdMS_TO_TICKS(3000));
				change_state(STATE_MENU);
			}
		} else if (check_timeout(VALIDATING_MAX_TIME_MS)) {
			show_error("Timeout validacion");
			vTaskDelay(pdMS_TO_TICKS(2000));
			change_state(STATE_MENU);
		}
		break;

	case STATE_DISPENSING:
		if (check_timeout(DISPENSE_MAX_TIME_MS)) {
			deactivate_dispenser();
			lcd_clear(lcd);
			lcd_set_cursor(lcd, 1, 0);
			lcd_write_string(lcd, "Tiempo maximo");
			lcd_set_cursor(lcd, 2, 0);
			lcd_write_string(lcd, "alcanzado");
			vTaskDelay(pdMS_TO_TICKS(2000));
			change_state(STATE_MENU);
		}
		break;

	default:
		break;
	}
}

void state_machine_key_pressed(char key) {
	switch (current_state) {

	case STATE_MENU:
		if (key == MENU_DNIPIN_SELECT_KEY) {
			reset_buffers();
			change_state(STATE_ENTER_DNI);
			ESP_LOGI(TAG, "Modo: DNI + PIN");
		}
		break;

	case STATE_ENTER_DNI:
		if (key == ENTER_DNI_ACCEPT_KEY) {
			if (dni_index == 8) {
				change_state(STATE_ENTER_PIN);
			} else {
				show_error("DNI debe tener 8 digitos.");
				vTaskDelay(pdMS_TO_TICKS(1500));
				show_enter_dni();
			}
		} else if (key == ENTER_DNI_CANCEL_KEY) {
			change_state(STATE_MENU);
		} else if (key == ENTER_DNI_DELETE_KEY && dni_index > 0) {
			// Borrar último dígito
			dni_index--;
			dni_buffer[dni_index] = '\0';
			show_enter_dni();
			state_start_time = xTaskGetTickCount();
		} else if (key >= '0' && key <= '9' && dni_index < 8) {
			dni_buffer[dni_index++] = key;
			show_enter_dni();
			state_start_time = xTaskGetTickCount();
		}
		break;

	case STATE_ENTER_PIN:
		if (key == ENTER_DNI_ACCEPT_KEY) {
			if (pin_index == 4) {
				change_state(STATE_VALIDATING);
				http_get_dni_pin_async(dni_buffer, pin_buffer);
			} else {
				show_error("PIN debe tener 4 digitos.");
				vTaskDelay(pdMS_TO_TICKS(1500));
				show_enter_pin();
			}
		} else if (key == ENTER_PIN_CANCEL_KEY) {
			pin_index = 0;
			memset(pin_buffer, 0, sizeof(pin_buffer));
			change_state(STATE_ENTER_DNI);
		} else if (key == ENTER_PIN_DELETE_KEY && pin_index > 0) {
			// Borrar último dígito
			pin_index--;
			pin_buffer[pin_index] = '\0';
			show_enter_pin();
			state_start_time = xTaskGetTickCount();
		} else if (key >= '0' && key <= '9' && pin_index < 4) {
			pin_buffer[pin_index++] = key;
			show_enter_pin();
			state_start_time = xTaskGetTickCount();
		}
		break;

	case STATE_SHOW_USER:
		if (key == SHOW_USER_EXTRACT_KEY && user_data.extracciones > 0) {
			change_state(STATE_DISPENSING);
		} else if (key == SHOW_USER_CANCEL_KEY) {
			change_state(STATE_MENU);
		}
		break;

	case STATE_DISPENSING:
		if (key == DISPENSING_STOP_KEY) {
			deactivate_dispenser();
			lcd_clear(lcd);
			lcd_set_cursor(lcd, 1, 0);
			lcd_write_string(lcd, "  Extraccion");
			lcd_set_cursor(lcd, 2, 0);
			lcd_write_string(lcd, "  completada!");
			vTaskDelay(pdMS_TO_TICKS(2000));
			change_state(STATE_MENU);
		}
		break;

	default:
		break;
	}
}

void state_machine_rfid_detected(const char *uid) {
	if (current_state == STATE_MENU) {
		ESP_LOGI(TAG, "RFID detectado: %s", uid);
		rc522_pause(*scanner);
		change_state(STATE_VALIDATING);
		http_get_uid_async(uid);
	}
}

void state_machine_validation_result(bool success, const char *nombre,
									 const char *apellido, int extracciones) {
	validation_complete = true;
	validation_success = success;

	if (success) {
		strncpy(user_data.nombre, nombre, sizeof(user_data.nombre) - 1);
		strncpy(user_data.apellido, apellido, sizeof(user_data.apellido) - 1);
		user_data.extracciones = extracciones;
		ESP_LOGI(TAG, "Usuario validado: %s %s - Extracciones: %d", nombre,
				 apellido, extracciones);
	}
}

system_state_t state_machine_get_state(void) { return current_state; }

// ============= FUNCIONES PRIVADAS =============

static void change_state(system_state_t new_state) {
	current_state = new_state;
	rc522_pause(*scanner);
	state_start_time = xTaskGetTickCount();
	// Resetear flag de validación al cambiar de estado
	if (new_state != STATE_VALIDATING) {
		validation_complete = false;
		validation_success = false;
	}

	// Mostrar la pantalla correspondiente al nuevo estado
	switch (new_state) {
	case STATE_MENU:
		show_menu();
		rc522_start(*scanner);
		break;
	case STATE_ENTER_DNI:
		show_enter_dni();
		break;
	case STATE_ENTER_PIN:
		show_enter_pin();
		break;
	case STATE_VALIDATING:
		show_validating();
		break;
	case STATE_SHOW_USER:
		show_user_info();
		break;
	case STATE_DISPENSING:
		show_dispensing();
		activate_dispenser();
		break;
	default:
		break;
	}
}

static bool check_timeout(TickType_t timeout_ms) {
	return (xTaskGetTickCount() - state_start_time) > pdMS_TO_TICKS(timeout_ms);
}

static void reset_buffers(void) {
	memset(dni_buffer, 0, sizeof(dni_buffer));
	memset(pin_buffer, 0, sizeof(pin_buffer));
	memset(&user_data, 0, sizeof(user_data));
	dni_index = 0;
	pin_index = 0;
	validation_complete = false;
	validation_success = false;
}

static void activate_dispenser(void) {
	gpio_set_level(RELE_GPIO, 1);
	ESP_LOGI(TAG, "Dispensador ACTIVADO");
}

static void deactivate_dispenser(void) {
	gpio_set_level(RELE_GPIO, 0);
	ESP_LOGI(TAG, "Dispensador DESACTIVADO");
}

static void show_menu(void) {
	lcd_clear(lcd);
	lcd_blink_off(lcd);
	lcd_set_cursor(lcd, 0, 0);
	lcd_write_string(lcd, "  DISPENSER AGUA");
	lcd_set_cursor(lcd, 1, 0);
	lcd_write_string(lcd, "--------------------");
	lcd_set_cursor(lcd, 2, 0);
	// lcd_write_string(lcd, "A: DNI + PIN");
	lcd_printf(lcd, "Acercar Tag RFID");
	lcd_set_cursor(lcd, 3, 0);
	lcd_printf(lcd, "%c: DNI + PIN", MENU_DNIPIN_SELECT_KEY);	
}


static void show_enter_dni(void) {
	lcd_clear(lcd);
	lcd_set_cursor(lcd, 0, 0);

	lcd_set_cursor(lcd, 2, 0);
	lcd_printf(lcd, "%c:Borrar %c:Aceptar", ENTER_DNI_DELETE_KEY,
			   ENTER_DNI_ACCEPT_KEY);
	lcd_set_cursor(lcd, 3, 0);
	lcd_printf(lcd, "%c:Cancelar", ENTER_DNI_CANCEL_KEY);
	
	lcd_set_cursor(lcd, 0, 0);
	lcd_write_string(lcd, "DNI:");
	lcd_set_cursor(lcd, 0, 5);
	lcd_write_string(lcd, dni_buffer);
	lcd_blink_on(lcd);
}

static void show_enter_pin(void) {
	lcd_clear(lcd);

	lcd_set_cursor(lcd, 2, 0);
    lcd_printf(lcd, "%c:Borrar %c:Aceptar", ENTER_PIN_DELETE_KEY,
			   ENTER_PIN_ACCEPT_KEY);
	lcd_set_cursor(lcd, 3, 0);
	lcd_printf(lcd, "%c:Cancelar", ENTER_PIN_CANCEL_KEY);

	lcd_set_cursor(lcd, 0, 0);
	lcd_write_string(lcd, "PIN:");
	lcd_set_cursor(lcd, 0, 5);
	lcd_write_string(lcd, pin_buffer);
	lcd_blink_on(lcd);
}

static void show_validating(void) {
	lcd_clear(lcd);
	lcd_blink_off(lcd);
	lcd_set_cursor(lcd, 1, 0);
	lcd_write_string(lcd, "  Validando...");
	lcd_set_cursor(lcd, 2, 0);
	lcd_write_string(lcd, "  Espere por favor");
}

static void show_user_info(void) {
	lcd_clear(lcd);
	lcd_set_cursor(lcd, 0, 0);
	lcd_printf(lcd, "%s %s", user_data.nombre, user_data.apellido);
	lcd_set_cursor(lcd, 1, 0);
	lcd_printf(lcd, "Extracciones: %d", user_data.extracciones);
	lcd_set_cursor(lcd, 2, 0);
	if (user_data.extracciones > 0) {
        lcd_printf(lcd, "%c:Iniciar extraccion", SHOW_USER_EXTRACT_KEY);
		lcd_set_cursor(lcd, 3, 0);
        lcd_printf(lcd, "%c:Salir ", SHOW_USER_CANCEL_KEY);
	} else {
		lcd_write_string(lcd, "Sin extracciones");
		lcd_set_cursor(lcd, 3, 0);
		lcd_printf(lcd, "%c:Cancelar", SHOW_USER_CANCEL_KEY);
	}
}

static void show_dispensing(void) {
	lcd_clear(lcd);
	lcd_set_cursor(lcd, 1, 0);
	lcd_write_string(lcd, "  DISPENSANDO AGUA");
	lcd_set_cursor(lcd, 2, 0);
	lcd_printf(lcd, "%c:Cancelar", DISPENSING_STOP_KEY);
}

static void show_error(const char *msg) {
	lcd_clear(lcd);
	lcd_blink_off(lcd);
	lcd_set_cursor(lcd, 0, 0);
	lcd_write_string(lcd, "    ERROR");
	lcd_write_auto(lcd, msg,2,0);

	// Doble beep de error
	setBuzzer(true);
	vTaskDelay(pdMS_TO_TICKS(100));
	setBuzzer(false);
	vTaskDelay(pdMS_TO_TICKS(100));
	setBuzzer(true);
	vTaskDelay(pdMS_TO_TICKS(100));
	setBuzzer(false);
}