#ifndef TIME_SYNC_H
#define TIME_SYNC_H
#include <time.h>
#include <stdbool.h>

/**
 * @brief Iniciar SNTP Time sync. Esta funcion sincroniza el tiempo del ESP32
 * con el del servidor NTP. Debe haber conexion a internet.
 */
void iniciar_sntp(void);

/**
 * @brief Obtener numero de dia  del anio, 0-365.
 * @return [int] Un entero con el numero de dia de
 * 0 a 365. En caso de error devuelve -1.
 */
int obtener_dia_anio_actual(void);
time_t obtener_timestamp(void);

#endif