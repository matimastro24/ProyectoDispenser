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

// Estructura de datos del usuario validado
typedef struct {
    char nombre[50];
    char apellido[50];
    int extracciones;
} user_data_t;

// Inicialización de la máquina de estados
void state_machine_init(lcd_t *lcd_ptr, rc522_handle_t *scanner_ptr);

// Actualización del estado (debe llamarse en el loop principal)
void state_machine_update(void);

// Callback para teclas presionadas
void state_machine_key_pressed(char key);

// Callback para RFID detectado
void state_machine_rfid_detected(const char *uid);

// Callback para resultado de validación
void state_machine_validation_result(bool success, const char* nombre, 
                                     const char* apellido, int extracciones);

// Obtener estado actual
system_state_t state_machine_get_state(void);
#endif