#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "rc522.h"
#include "rc522_picc.h"

// Estados del sistema
typedef enum {
    STATE_MENU,           // Menú inicial
    STATE_ENTER_DNI,      // Ingresando DNI
    STATE_ENTER_PIN,      // Ingresando PIN
    STATE_VALIDATING,     // Validando con base de datos
    STATE_SHOW_USER,      // Mostrando datos del usuario
    STATE_DISPENSING,     // Dispensando agua
    STATE_ERROR           // Error de validación
} system_state_t;

/**
 * @brief Inicializacion de maquina de estados.
 */
void state_machine_init();

/**
 * @brief Actualiza los estados que dependen del tiempo.
 */
void state_machine_update(void);

/**
 * @brief Llamar a esta funcion cuando se detecta una tecla pulsada
 * para actualizar la maquina de estados.
 * @param key [in] Tecla pulsada.
 */
void state_machine_key_pressed(char key);

/**
 * @brief Llamar a esta funcion cuando se detecta un RFID,
 * desde el event handler del sensor. La funcion modifica
 * la bandera rfid_flag para que la maquina de estados sepa
 * que se detecto una tarjeta RFID.
 * @param uid [in] UID Detectado.
 */
void state_machine_rfid_detected(uint32_t uid);

/**
 * @brief Obtener estado actual de la maquina de estados.
 */
system_state_t state_machine_get_state(void);

#endif