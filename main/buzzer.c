#include <stddef.h>
#include "buzzer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#define BUZZER_GPIO  4

void buzzer(void){
	gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
}

void setBuzzer(bool estado){
	if(estado) gpio_set_level(BUZZER_GPIO, 1);
	if(!estado) gpio_set_level(BUZZER_GPIO, 0);
}

void buzzer_task(void *pvParameters)
{
    gpio_set_level(BUZZER_GPIO, 1);

    vTaskDelay(pdMS_TO_TICKS(250));  // espera 250 ms

    // apagar buzzer
    gpio_set_level(BUZZER_GPIO, 0);

    vTaskDelete(NULL);  // elimina la propia tarea
}