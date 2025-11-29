#include "time_sync.h"
#include "esp_sntp.h"
#include <esp_log.h>
#include <sys/time.h>
#include <time.h>
static const char *TAG = "TIME_SYNC";

void iniciar_sntp(void) {
	ESP_LOGI(TAG, "Iniciando SNTP...");
	esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
	esp_sntp_setservername(0, "pool.ntp.org");
	esp_sntp_init();
	// Configurar Zona Horaria para Argentina (UTC-3)
	setenv("TZ", "ART3", 1);
	tzset();
}
int obtener_dia_anio_actual(void) {
	time_t now;			// long con los segundos q pasaron desde el 1/1/1970.
	struct tm timeinfo; // estructura con año, mes, día, hora, minuto.
	time(&now);			// actualiza now.
	localtime_r(&now, &timeinfo); // toma los segundos y rellena la estructura.

	// tm_year anios transcurridos desde 1900
	if (timeinfo.tm_year < (2020 - 1900)) {
		// en este caso ya se descarta la infor del tiempo
		return -1; // Error: No hay hora sincronizada
	}
	return timeinfo.tm_yday; // Retorna 0-365
}

// Función para obtener Timestamp Unix (para guardar en el log)
time_t obtener_timestamp(void) {
	time_t now;
	time(&now);
	return now;
}
