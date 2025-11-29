#include "state_machine.h"
#include "buzzer.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_server.h"
#include "keypad.h"
#include "local_db.h"
#include "rfid.h"
#include "time_sync.h"
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
static rc522_driver_handle_t driver;
static rc522_handle_t scanner;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t lcd_dev;
static lcd_t lcd;
static system_state_t current_state = STATE_MENU;

// Buffers para el ingreso por teclado.
static char dni_buffer[9] = {0};
static char pin_buffer[5] = {0};
static uint8_t dni_index = 0;
static uint8_t pin_index = 0;
// Flag para cuando se detecta RFID
static bool rfid_flag = false;
// Valores obtenidos desde la base de datos.
static int user_index_db = -2;
static uint32_t dni_numero;
static uint16_t pin_numero;
static uint32_t uid_numero;
static int extraciones_restantes;
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
static void rele_init(void);
static void tarea_sincronizacion_background(void *pvParameters);
// Funciones publicas

static bool ram_usos_diarios_sincronizada = false;

void state_machine_init() {
	// Inicio de componentes.
	ESP_LOGI(TAG, "Iniciando perifericos.");
	i2c_init(&i2c_bus, &lcd_dev);
	lcd_init(&lcd, lcd_dev);
	lcd_clear(&lcd);
	lcd_write_string(&lcd, "Iniciando Sistema...");
	rfid_init(&driver, &scanner);
	buzzer();
	keypad_init();
	rele_init();
	init_littlefs();
	wifi_init_apsta();
	http_server_start();
	iniciar_sntp();
	// Arranca directamente con la db local.
	cargar_db_a_ram_stream();
	/**
	 * tarea_sincronizacion_background Es una tarea es para revisar si hay
	 * actualizaciones pendientes. Si detecta que hay una actulaizazacion
	 * (version_nube > version_local) la descarga en la flash y espera a que
	 * este en el menu principal para reemplazarla en la ram. Cuando esta
	 * funcionando alerda todo lo demas. Capaz hay que hacerla de otra forma o
	 * aumentar el tiempo de cada cuanto se hace, el taskdelay.
	 */
	xTaskCreatePinnedToCore(
		tarea_sincronizacion_background,
		"SyncTask", // Nombre para debug
		8192,		// Stack (Memoria RAM asignada a esta tarea)
			  // 8KB es necesario para HTTPS/SSL. Si pones menos, explotará.
		NULL, // Parámetros (no usamos)
		1,	  // Prioridad (1 = Baja, 5 = Alta).
			  // Ponemos 1 para que NO interrumpa al lector RFID.
		NULL, // Handle (no necesitamos guardarlo)
		0	  // Core ID: 0 (Background) o 1 (Main/Arduino)
	);
	ESP_LOGI(TAG, "Máquina de estados inicializada");
	change_state(STATE_MENU);
}

void state_machine_update(void) {
	switch (current_state) {
	case STATE_MENU:
		if (!ram_usos_diarios_sincronizada) {
			if (recuperar_usos_del_dia() == ESP_OK) {
				ram_usos_diarios_sincronizada = true;
				show_menu();
			}
		}
		if (rfid_flag) {
			rfid_flag = false;
			/*
			 * Aca ya tenemos RFID. Ahora hay que buscarlo en la DB.
			 */
			change_state(STATE_VALIDATING);
		}
		break;
	case STATE_ENTER_DNI:
	case STATE_ENTER_PIN:
	case STATE_SHOW_USER:
		/*
		 * en estos estados, si el user no pulsa niguna tecla en un tiempo
		 * entonces se vuelve al menu, se cencela la sesion por timeout.
		 */
		if (check_timeout(TIMEOUT_USER_INPUT_MS)) {
			show_error("Timeout");
			vTaskDelay(pdMS_TO_TICKS(2000));
			change_state(STATE_MENU);
		}
		break;
	case STATE_VALIDATING:
		if (dni_index != 0) {
			// en este caso el metodo de acceso es dni+pin
			dni_numero = (uint32_t)strtoul(dni_buffer, NULL, 10);
			pin_numero = (uint16_t)strtoul(pin_buffer, NULL, 10);
			user_index_db = buscar_usuario_por_dni_pin(dni_numero, pin_numero);
		} else {
			user_index_db = buscar_usuario_por_rfid(uid_numero);
		}
		switch (user_index_db) {
		case -2:
			show_error("DNI-OK-PIN-NO");
			change_state(STATE_MENU);
			break;
		case -1:
			show_error("USUARIO NO ENCONTRADO");
			change_state(STATE_MENU);
			break;
		default:
			extraciones_restantes =
				extraciones_diarias_restantes(user_index_db);
			change_state(STATE_SHOW_USER);
			break;
		}
		break;
	case STATE_DISPENSING:
		if (check_timeout(DISPENSE_MAX_TIME_MS)) {
			deactivate_dispenser();
			lcd_clear(&lcd);
			lcd_set_cursor(&lcd, 1, 0);
			lcd_write_string(&lcd, "Tiempo maximo");
			lcd_set_cursor(&lcd, 2, 0);
			lcd_write_string(&lcd, "alcanzado");
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
				/*
				 * Aca ya tenemos DNI y PIN. Ahora hay que buscar en la DB.
				 */
				change_state(STATE_VALIDATING);
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
		if (extraciones_restantes > 0 && key == SHOW_USER_EXTRACT_KEY) {
			if (obtener_dia_anio_actual() != -1) {
				db_usuarios[user_index_db].usos_hoy++;
				registrar_extraccion_flash(db_usuarios[user_index_db].dni);
			}
			change_state(STATE_DISPENSING);
		} else if (key == SHOW_USER_CANCEL_KEY) {
			change_state(STATE_MENU);
		}

		break;

	case STATE_DISPENSING:
		if (key == DISPENSING_STOP_KEY) {
			deactivate_dispenser();
			lcd_clear(&lcd);
			lcd_set_cursor(&lcd, 1, 0);
			lcd_write_string(&lcd, "  Extraccion");
			lcd_set_cursor(&lcd, 2, 0);
			lcd_write_string(&lcd, "  completada!");
			vTaskDelay(pdMS_TO_TICKS(2000));
			change_state(STATE_MENU);
		}
		break;

	default:
		break;
	}
}

void state_machine_rfid_detected(uint32_t uid) {
	if (current_state == STATE_MENU) {
		ESP_LOGI(TAG, "RFID Numérico: %" PRIu32 " (Hex: %X)", uid, uid);
		uid_numero = uid;
		rfid_flag = true;
	}
}

system_state_t state_machine_get_state(void) { return current_state; }

// ============= FUNCIONES PRIVADAS =============

/*
cambia el estado de la maquina de estados
*/
static void change_state(system_state_t new_state) {

	if (new_state != STATE_MENU) {
		rc522_pause(scanner);
	}
	current_state = new_state;
	state_start_time = xTaskGetTickCount();
	// Mostrar la pantalla correspondiente al nuevo estado
	switch (new_state) {
	case STATE_MENU:
		show_menu();
		rc522_start(scanner);
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
	dni_index = 0;
	pin_index = 0;
	user_index_db = -2;
	dni_numero = 0;
	pin_numero = 0;
	uid_numero = 0;
	rfid_flag = false;
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
	lcd_clear(&lcd);
	lcd_blink_off(&lcd);
	lcd_set_cursor(&lcd, 0, 0);
	lcd_write_string(&lcd, "   DISPENSER AGUA");
	lcd_set_cursor(&lcd, 1, 0);
	lcd_write_string(&lcd, "--------------------");
	lcd_set_cursor(&lcd, 2, 0);
	// lcd_write_string(&lcd, "A: DNI + PIN");
	lcd_printf(&lcd, "  Acercar Tag RFID");
	lcd_set_cursor(&lcd, 3, 0);
	lcd_printf(&lcd, "%c: DNI + PIN     ", MENU_DNIPIN_SELECT_KEY);
	lcd_printf(&lcd, "%d", obtener_dia_anio_actual());
}

static void show_enter_dni(void) {
	lcd_clear(&lcd);
	lcd_set_cursor(&lcd, 0, 0);

	lcd_set_cursor(&lcd, 2, 0);
	lcd_printf(&lcd, "%c:Borrar %c:Aceptar", ENTER_DNI_DELETE_KEY,
			   ENTER_DNI_ACCEPT_KEY);
	lcd_set_cursor(&lcd, 3, 0);
	lcd_printf(&lcd, "%c:Cancelar", ENTER_DNI_CANCEL_KEY);

	lcd_set_cursor(&lcd, 0, 0);
	lcd_write_string(&lcd, "DNI:");
	lcd_set_cursor(&lcd, 0, 5);
	lcd_write_string(&lcd, dni_buffer);
	lcd_blink_on(&lcd);
}
static void show_enter_pin(void) {
	lcd_clear(&lcd);
	lcd_set_cursor(&lcd, 2, 0);
	lcd_printf(&lcd, "%c:Borrar %c:Aceptar", ENTER_PIN_DELETE_KEY,
			   ENTER_PIN_ACCEPT_KEY);
	lcd_set_cursor(&lcd, 3, 0);
	lcd_printf(&lcd, "%c:Cancelar", ENTER_PIN_CANCEL_KEY);
	lcd_set_cursor(&lcd, 0, 0);
	lcd_write_string(&lcd, "PIN:");
	lcd_set_cursor(&lcd, 0, 5);
	lcd_write_string(&lcd, pin_buffer);
	lcd_blink_on(&lcd);
}
static void show_validating(void) {
	lcd_clear(&lcd);
	lcd_blink_off(&lcd);
	lcd_set_cursor(&lcd, 1, 0);
	lcd_write_string(&lcd, "  Validando...");
	lcd_set_cursor(&lcd, 2, 0);
	lcd_write_string(&lcd, "  Espere por favor");
}
static void show_user_info(void) {
	ESP_LOGI(TAG, "Inicio show user");
	lcd_clear(&lcd);
	lcd_set_cursor(&lcd, 0, 0);
	lcd_printf(&lcd, "Diarias: %i", db_usuarios[user_index_db].extracciones);
	lcd_set_cursor(&lcd, 1, 0);
	lcd_printf(&lcd, "Restantes: %i", extraciones_restantes);
	lcd_set_cursor(&lcd, 2, 0);
	if (extraciones_restantes > 0) {
		lcd_printf(&lcd, "%c:Iniciar extraccion", SHOW_USER_EXTRACT_KEY);
		lcd_set_cursor(&lcd, 3, 0);
		lcd_printf(&lcd, "%c:Salir ", SHOW_USER_CANCEL_KEY);
	} else {
		lcd_write_string(&lcd, "Sin extracciones");
		lcd_set_cursor(&lcd, 3, 0);
		lcd_printf(&lcd, "%c:Cancelar", SHOW_USER_CANCEL_KEY);
	}
	ESP_LOGI(TAG, "Fin show user");
}
static void show_dispensing(void) {
	lcd_clear(&lcd);
	lcd_set_cursor(&lcd, 1, 0);
	lcd_write_string(&lcd, "  DISPENSANDO AGUA");
	lcd_set_cursor(&lcd, 2, 0);
	lcd_printf(&lcd, "%c:Cancelar", DISPENSING_STOP_KEY);
}
static void show_error(const char *msg) {
	lcd_clear(&lcd);
	lcd_blink_off(&lcd);
	lcd_set_cursor(&lcd, 0, 0);
	lcd_write_string(&lcd, "    ERROR");
	lcd_write_auto(&lcd, msg, 2, 0);

	// Doble beep de error
	setBuzzer(true);
	vTaskDelay(pdMS_TO_TICKS(100));
	setBuzzer(false);
	vTaskDelay(pdMS_TO_TICKS(100));
	setBuzzer(true);
	vTaskDelay(pdMS_TO_TICKS(100));
	setBuzzer(false);
}

static void rele_init(void) {
	// Inicializar GPIO del relé
	gpio_config_t io_conf = {.pin_bit_mask = (1ULL << RELE_GPIO),
							 .mode = GPIO_MODE_OUTPUT,
							 .pull_up_en = GPIO_PULLUP_DISABLE,
							 .pull_down_en = GPIO_PULLDOWN_DISABLE,
							 .intr_type = GPIO_INTR_DISABLE};
	ESP_ERROR_CHECK(gpio_config(&io_conf));
	ESP_ERROR_CHECK(gpio_set_level(RELE_GPIO, 0));
}

static const char *TAG_SYNC = "BACKGROUND_TASK";

static void tarea_sincronizacion_background(void *pvParameters) {

	// 1. INICIALIZAR DRIVER WIFI (Una única vez)
	// Asumimos que wifi_init_sta() configura el driver pero NO bloquea.
	// Si tu wifi_init_sta() tiene un wait loop infinito, quítalo.
	// wifi_init_sta();
	// iniciar_sntp();
	// BUCLE INFINITO DE MANTENIMIENTO
	while (1) {
		vTaskDelay(pdMS_TO_TICKS(10000));
		ESP_LOGI(TAG_SYNC, "Arrancando servicio de sincronización...");
		if (obtener_dia_anio_actual() != -1) { // Espera activa de hasta 10 seg
			if (existe_archivo("/littlefs/cambios.bin")) {
				/**
				 * Antes de purgar los logs viejo se podrian subir a otra hoja de datos
				 * para tener registro permanente de las extracciones. No esta nada implementado.
				 */
				purgar_logs_viejos();
			}
		}

		if (wifi_is_connected()) {
			ESP_LOGW(TAG_SYNC, "Hay conexión WIFI. ");

			// Leer versiones.
			uint16_t version_local = leer_version_local();
			uint16_t version_nube = obtener_version_nube();

			if (version_nube > 0 && version_local > 0) {
				// se pudieron leer ambas veriones
				if (version_nube > version_local) {
					ESP_LOGI(
						TAG_SYNC,
						"Actualización encontrada: Nube V%d > Local V%" PRIu32,
						version_nube, version_local);

					// A. Descarga a Flash (Lenta, pero segura, no toca RAM aun)
					if (descargar_base_datos() == ESP_OK) {

						ESP_LOGI(
							TAG_SYNC,
							"Descarga completa. Esperando acceso a RAM...");

						// B. Actualización de RAM. Se hace solo cuando se esta
						// en el menu.
						// para que la maquina de estados no este utilizando la
						// db acutal y poder cambiarla.
						if (state_machine_get_state() == STATE_MENU) {
							ESP_LOGI(TAG_SYNC, "Actualizando ram");
							cargar_db_a_ram_stream();
							recuperar_usos_del_dia();

							// Confirmar versión nueva en NVS
							guardar_version_local((uint32_t)version_nube);
							ESP_LOGI(TAG_SYNC,
									 "RAM Actualizada exitosamente a V%d",
									 version_nube);
						} else {
							ESP_LOGE(TAG_SYNC,
									 "Fuera del menu, no se actuliza ram.");
						}
					} else {
						ESP_LOGE(TAG_SYNC, "Fallo la descarga del JSON. "
										   "Integridad NVS intacta.");
					}
				} else {
					ESP_LOGD(TAG_SYNC, "Sistema actualizado.", version_local);
					ESP_LOGW(TAG_SYNC, "VERSION LOCAL: %" PRIu32 ".",
							 version_local);
					ESP_LOGW(TAG_SYNC, "VERSION NUBE: %" PRIu32 ".",
							 version_nube);
				}
			} else {
				ESP_LOGW(TAG_SYNC, "ERROR OBTENIENDO VERSIONES.");
				ESP_LOGW(TAG_SYNC, "VERSION LOCAL: %" PRIu32 ".",
						 version_local);
				ESP_LOGW(TAG_SYNC, "VERSION NUBE: %" PRIu32 ".", version_nube);
			}
		}
		ESP_LOGI(TAG_SYNC, "Finaliza tarea de sincronizacion.");
		vTaskDelay(pdMS_TO_TICKS(100000));
	}
}
