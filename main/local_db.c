#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "local_db.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "time_sync.h"
#include <unistd.h>
/*
https://script.google.com/macros/s/AKfycbwWRQocdLq13cf1czfCb8BNM0pRrFgPVpoc2TCTqiJtHO3_astKrJcsP_DZ13osDn_vVg/exec
*/
static const char *TAG = "LOCAL_DB";

usuario_t db_usuarios[MAX_USUARIOS];
int cantidad_usuarios_actual = 0;

esp_err_t init_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "db_storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    // Use settings defined above to initialize and mount LittleFS filesystem.
    // Note: esp_vfs_littlefs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "No se pudo montar ni formatear el sistema de archivos.");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "No se pudo encontrar la partición LittleFS.");
        }
        else
        {
            ESP_LOGE(TAG, "No se pudo inicializar LittleFS (%s).", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ret;
}

/**
 * @brief Parsea un unico usuario y lo agrega a la DB en RAM.
 * @param json_str String json del usuario.
 * @return - ESP-OK 
 * @return - ESP-FAIL Json pareseado resulto NULL. 
 */
esp_err_t procesar_usuario_individual(char *json_str) {
    // Parseamos solo este string pequeño.
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) return ESP_FAIL; // Si falló el parseo, salimos

    usuario_t *u = &db_usuarios[cantidad_usuarios_actual];

    // 1. EXTRAER DNI
    cJSON *j_dni = cJSON_GetObjectItemCaseSensitive(root, "dni");
    u->dni = cJSON_IsNumber(j_dni) ? (uint32_t)j_dni->valueint : 0;

    // 2. EXTRAER PIN
    cJSON *j_pin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    u->pin = cJSON_IsNumber(j_pin) ? (uint16_t)j_pin->valueint : 0;

    // 3. EXTRAER EXTRACCIONES (Con manejo de errores string/int)
    cJSON *j_ext = cJSON_GetObjectItemCaseSensitive(root, "extracciones");
    if (cJSON_IsNumber(j_ext)) {
        u->extracciones = (uint8_t)j_ext->valueint;
    } else if (cJSON_IsString(j_ext)) {
        u->extracciones = (uint8_t)atoi(j_ext->valuestring);
    } else {
        u->extracciones = 0;
    }

    // 4. EXTRAER Y CONVERTIR RFID (Hex String -> uint32)
    cJSON *j_rfid = cJSON_GetObjectItemCaseSensitive(root, "rfid");
    if (cJSON_IsString(j_rfid)) {
        // strtoul convierte "DA983C03" -> 3667377155
        // Base 16 es clave aquí
        u->rfid_val = (uint32_t)strtoul(j_rfid->valuestring, NULL, 16);
    } else {
        u->rfid_val = 0;
    }

    // Inicializamos los contadores siempre en limpio
    u->usos_hoy = 0;
    u->ultimo_dia = -1;

    // 5. CONFIRMAR Y LIMPIAR
    cJSON_Delete(root); // ¡Importante! Liberamos la mini-estructura cJSON
    
    // Solo aumentamos el contador si el usuario parece válido (ej: tiene DNI o RFID)
    if (u->dni != 0 || u->rfid_val != 0) {
        cantidad_usuarios_actual++;
    }
    return ESP_OK;
}
esp_err_t cargar_db_a_ram_stream() {
    ESP_LOGI(TAG, "Iniciando carga Stream (Lite)...");

    FILE *f = fopen("/littlefs/usuarios.json", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "No se encontro usuarios.json");
        return ESP_FAIL;
    }

    // Buffer pequeño para guardar temporalmente el texto de 1 usuario
    char user_buffer[JSON_USER_BUFFER_SIZE];
    int buf_index = 0;
    
    // Banderas de estado
    bool capturando = false;
    int llaves_abiertas = 0; // Para manejar anidamiento si hubiera
    char c;

    cantidad_usuarios_actual = 0;

    // Leemos el archivo byte por byte (buffered internamente por el sistema)
    while (fread(&c, 1, 1, f) == 1) {
        
        // Si alcanzamos el límite de usuarios, cortamos
        if (cantidad_usuarios_actual >= MAX_USUARIOS) break;

        // 1. DETECTAR INICIO DE USUARIO '{'
        if (c == '{') {
            if (!capturando) {
                capturando = true;
                buf_index = 0; // Reiniciar buffer
                llaves_abiertas = 0;
            }
            llaves_abiertas++;
        }

        // 2. GUARDAR CARACTERES
        if (capturando) {
            if (buf_index < JSON_USER_BUFFER_SIZE - 1) {
                user_buffer[buf_index++] = c;
            } else {
                // Seguridad: Si el usuario es gigante y llena el buffer, abortamos este usuario
                ESP_LOGW(TAG, "Usuario excede tamaño de buffer, omitiendo...");
                capturando = false; 
                continue;
            }
        }

        // 3. DETECTAR FIN DE USUARIO '}'
        if (c == '}') {
            if (capturando) {
                llaves_abiertas--;
                if (llaves_abiertas == 0) {
                    // ¡Tenemos un objeto JSON completo!
                    user_buffer[buf_index] = '\0'; // Terminador nulo
                    
                    // PROCESAMOS ESTE PEDACITO
                    if (procesar_usuario_individual(user_buffer) != ESP_OK){
                        ESP_LOGE(TAG, "ERROR PARSEANDO USER.");
                    }
                    capturando = false; // Listos para buscar el siguiente
                }
            }
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Carga finalizada. Total RAM: %d usuarios.", cantidad_usuarios_actual);
    return ESP_OK;
}
void imprimir_db(void){
    for (int i = 0; i < cantidad_usuarios_actual; i++)
    {
        printf("RFID: %" PRIu32 "-", db_usuarios[i].rfid_val);
        printf("DNI: %" PRIu32 "-", db_usuarios[i].dni);
        printf("PIN: %" PRIu16 "-", db_usuarios[i].pin);
        printf("EXTS: %" PRIu8 "\n", db_usuarios[i].extracciones);
        printf("\n");

        // evitar desborde de watchdog
        // Cada 20 usuarios, hacemos una pausa de 10 milisegundos.
        // Esto permite que el Watchdog se resetee y el WiFi no se desconecte.
        if (i % 20 == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }   
}
int buscar_usuario_por_dni_pin(uint32_t dni_ingresado, uint16_t pin_ingresado) {
    for (int i = 0; i < cantidad_usuarios_actual; i++) {
        if (db_usuarios[i].dni == dni_ingresado) {
            // El DNI existe, ahora miramos el PIN
            if (db_usuarios[i].pin == pin_ingresado) {
                return i; // ¡Éxito!
            } else {
                ESP_LOGW(TAG, "DNI %" PRIu32 " encontrado pero PIN incorrecto", dni_ingresado);
                return -2; // Error de PIN
            }
        }
    }
    return -1; // DNI no existe
}

int buscar_usuario_por_dni(uint32_t dni_ingresado) {
    for (int i = 0; i < cantidad_usuarios_actual; i++) {
        if (db_usuarios[i].dni == dni_ingresado) {
            // El DNI existe, ahora miramos el PIN
            return i; // ¡Éxito!
        }
    }
    return -1; // DNI no existe
}


int buscar_usuario_por_rfid(uint32_t rfid_leido) {
    // Si el RFID es 0, es inválido, salimos rápido
    if (rfid_leido == 0) return -1;

    for (int i = 0; i < cantidad_usuarios_actual; i++) {
        // Comparación directa de números (muy rápida)
        if (db_usuarios[i].rfid_val == rfid_leido) {
            ESP_LOGI(TAG, "RFID %" PRIu32 " ENCONTRADO.", rfid_leido);
            return i; // ¡Encontrado! Retornamos su posición
        }
    }
    ESP_LOGI(TAG, "RFID %" PRIu32 " NO ENCONTRADO.", rfid_leido);
    return -1; // Recorrimos todo y no apareció
}

int extraciones_diarias_restantes(int index) {
    usuario_t *u = &db_usuarios[index];
    int dia_hoy = obtener_dia_anio_actual(); // Requiere NTP
    //si no se pudo sincronizar el dia devuelve como restantes el total disponible.
    if (dia_hoy == -1){
        return u->extracciones; 
    }
    // 1. LAZY RESET (Si cambió el día, reseteamos el contador)
    if (u->ultimo_dia != dia_hoy) {
        u->usos_hoy = 0;        // ¡Volvemos a cero! Es un nuevo día.
        u->ultimo_dia = dia_hoy;
    }

    return u->extracciones - u->usos_hoy;
}

esp_err_t registrar_extraccion_flash(uint32_t dni) {
    registro_acceso_t reg;
    reg.dni = dni;
    reg.timestamp = obtener_timestamp();
    FILE *f = fopen("/littlefs/cambios.bin", "ab"); // Append Binary
    if (f) {
        fwrite(&reg, sizeof(registro_acceso_t), 1, f);
        ESP_LOGI(TAG, "Extracion registrada en flash.");
        fclose(f);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t recuperar_usos_del_dia(void) {
    ESP_LOGI(TAG, "Recuperando historial del día...");
    int16_t recuperados = 0; 
    FILE *f = fopen("/littlefs/cambios.bin", "rb");
    if (!f) return ESP_FAIL;

    int dia_hoy = obtener_dia_anio_actual();
    if (dia_hoy == -1) {
        fclose(f);
        ESP_LOGI(TAG, "No se pudo obetener hora. No se pueden recuperar los usos del dia.");
        return ESP_FAIL; // Si no hay hora, no podemos saber qué registros son de hoy
    }

    registro_acceso_t reg;
    while(fread(&reg, sizeof(registro_acceso_t), 1, f) == 1) {
        
        // 1. ¿Este registro es de HOY?
        struct tm timeinfo;
        localtime_r(&reg.timestamp, &timeinfo);
        
        if (timeinfo.tm_yday == dia_hoy) {
            // 2. Buscar usuario en RAM y sumar
            // (Aquí podrías optimizar buscando por DNI, asumimos que tienes una función buscar)
            int idx = buscar_usuario_por_dni(reg.dni); 
            
            if (idx >= 0) {
                db_usuarios[idx].ultimo_dia = dia_hoy;
                db_usuarios[idx].usos_hoy++;
                recuperados++;
            }
        }
    }
    ESP_LOGI(TAG, "Usos en el dia recuperados: %d.", recuperados);
    fclose(f);
    return ESP_OK;
}

bool existe_archivo(const char *path) {
    // Intentamos abrir en modo lectura ("r")
    FILE *f = fopen(path, "r");
    
    if (f == NULL) {
        return false; // No se pudo abrir (probablemente no existe)
    }
    
    // Si se abrió, es muy importante cerrarlo inmediatamente
    fclose(f);
    return true;
}

void purgar_logs_viejos() {
    ESP_LOGI(TAG, "Iniciando limpieza de logs antiguos...");

    // 1. OBTENER EL DÍA ACTUAL (Referencia)
    // Es crítico tener la hora sincronizada. Si falla, abortamos para no borrar datos por error.
    int dia_hoy = obtener_dia_anio_actual();
    if (dia_hoy == -1) {
        ESP_LOGE(TAG, "Error: No hay hora NTP. Se aborta la purga por seguridad.");
        return;
    }

    // 2. ABRIR ARCHIVOS
    FILE *f_origen = fopen("/littlefs/cambios.bin", "rb");
    if (f_origen == NULL) {
        ESP_LOGW(TAG, "No existe el archivo de logs. Nada que purgar.");
        return;
    }

    FILE *f_destino = fopen("/littlefs/temp_logs.bin", "wb");
    if (f_destino == NULL) {
        ESP_LOGE(TAG, "Error al crear archivo temporal. Abortando.");
        fclose(f_origen);
        return;
    }

    // 3. FILTRADO (Streaming)
    registro_acceso_t registro;
    int conservados = 0;
    int eliminados = 0;
    struct tm timeinfo;

    // Leemos struct por struct
    while (fread(&registro, sizeof(registro_acceso_t), 1, f_origen) == 1) {
        
        // Convertimos el timestamp del registro a fecha legible
        localtime_r(&registro.timestamp, &timeinfo);

        // CONDICIÓN DE FILTRADO:
        // ¿El día del año coincide con hoy?
        //También chequeamos el año para evitar bugs de año nuevo (31 dic vs 1 ene)
        // Pero para simplificar, comparar tm_yday suele ser suficiente si purgas a diario.
        
        if (timeinfo.tm_yday == dia_hoy) {
            // ES DE HOY -> LO GUARDAMOS
            fwrite(&registro, sizeof(registro_acceso_t), 1, f_destino);
            conservados++;
        } else {
            // ES VIEJO -> LO DESCARTAMOS (No escribimos nada)
            eliminados++;
        }
    }

    // 4. CIERRE Y REEMPLAZO ATÓMICO
    fclose(f_origen);
    fclose(f_destino);

    // Si borramos el original y renombramos el temporal, logramos la actualización
    unlink("/littlefs/cambios.bin"); // Borrar viejo
    
    if (rename("/littlefs/temp_logs.bin", "/littlefs/cambios.bin") == 0) {
        ESP_LOGI(TAG, "Purga exitosa. Conservados (Hoy): %d | Eliminados (Viejos): %d", conservados, eliminados);
    } else {
        ESP_LOGE(TAG, "Fallo al renombrar el archivo temporal.");
    }
}