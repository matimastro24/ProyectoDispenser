#include <stddef.h>
#include "buzzer.h"
#include "esp_err.h"

#include "driver/gpio.h"

#define BUZZER_GPIO  4

void buzzer(void){
	gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
}

void setBuzzer(bool estado){
	if(estado) gpio_set_level(BUZZER_GPIO, 1);
	if(!estado) gpio_set_level(BUZZER_GPIO, 0);
}