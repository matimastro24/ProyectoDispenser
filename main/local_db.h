
#ifndef LOCAL_DB_H
#define LOCAL_DB_H

// Configuración de límites
#define MAX_USUARIOS 3000
#define JSON_USER_BUFFER_SIZE 256
#include <time.h>
// Estructura de Usuario en RAM
typedef struct {
	uint32_t rfid_val;	  // 4 bytes (El RFID convertido a número)
	uint32_t dni;		  // 4 bytes
	uint16_t pin;		  // 2 bytes
	uint8_t extracciones; // 1 byte limite diario de extraccioens
	uint8_t usos_hoy;	  // Contador actual (RAM)
	int16_t ultimo_dia;	  // El día del año del último uso (0-365)
} usuario_t;

typedef struct {
	uint32_t dni;
	time_t timestamp; // Momento exacto del acceso
} registro_acceso_t;

// VARIABLES GLOBALES (La "Base de Datos" en RAM)
extern usuario_t db_usuarios[MAX_USUARIOS];
extern int cantidad_usuarios_actual;

/**
 * @brief Iniciar sistema de archivos LittleFS.
 * @return - ESP-OK LittleFS inicializado.
 * @return - ESP-FAIL LittleFS no se pudo iniciar.
 */
esp_err_t init_littlefs(void);
/**
 * @brief Carga la DB de la FLASH en la RAM.
 * @return - ESP-OK
 * @return - ESP-FAIL No se encontro usuarios.json en flash.
 */
esp_err_t cargar_db_a_ram_stream(void);

/**
 * @brief Imprime la DB en el monitor serial.
 */
void imprimir_db(void);
/**
 * @brief Busca un usurio en DB a partir de su DNI y valida el PIN.
 * @param dni_ingreso DNI buscado.
 * @param pin_ingresado PIN.
 * @return - DNI y PIN correctos - Index en la DB del usuario.
 * @return - -1: DNI no encontrado en la DB.
 * @return - -2: DNI encontrado, pero PIN incorrecto.
 */
int buscar_usuario_por_dni_pin(uint32_t dni_ingresado, uint16_t pin_ingresado);
/**
 * @brief Busca un usurio a paritr de su DNI.
 * @param dni_ingreso DNI buscado.
 * @return - Index en la DB del usuario.
 * @return - -1: DNI no encontrado en la DB.
 */
int buscar_usuario_por_dni(uint32_t dni_ingresado);
/**
 * @brief Busca un usurio a paritr de su RFID.
 * @param rfid_leido RFID.
 * @return - Index en la DB del usuario.
 * @return - -1: RFID no encontrado en la DB.
 */
int buscar_usuario_por_rfid(uint32_t rfid_leido);
/**
 * @brief Extraciones diarias restantes del usuario. Esta funcion
 * dpende de obtener_dia_anio_actual(), por lo tanto requiere que de la
 * sincronizacion temporal NTP. Si no esta sincronizado, devuelve la cantidad
 * total de extraciones diarias del usuario.
 * @param index Indice del usurio en la DB.
 * @return - Extracciones restantes.
 */
int extraciones_diarias_restantes(int index);
/**
 * @brief Registrar la extracion del usuario en la memoria flash.
 * @param dni DNI del usuario.
 * @return - ESP-OK.
 * @return - ESP-FAIL.
 */
esp_err_t registrar_extraccion_flash(uint32_t dni);

/**
 * @brief Recupera la usos diarios guarados en la flash (cambios.bin).
 * El proposito de esta funcion es poder recuperar los usos en el dia en caso de
 * que el ESP32 se reinicie, ya que los cambios se guardan en la db de la ram. 
 * Esta funcion dpende de de obtener el dia actual, por lo tanto requiere que de 
 * la sincronizacion temporal NTP. Si no esta sincronizado, devuelve ESP-FAIL.
 * @return - ESP-OK.
 * @return - ESP-FAIL.
 */
esp_err_t recuperar_usos_del_dia(void);

bool existe_archivo(const char *path);
/**
 * @brief Limpia logs viejos en el archivo de cambios.bin. Necesita tener el
 * dia sincronizado.
 * Esta es la unica forma para limitar el crecimiento de cambios.bin.
 */
void purgar_logs_viejos(void);
#endif // LOCAL_DB_H