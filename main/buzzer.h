#pragma once
#include <stddef.h>
#include "esp_err.h"

#include "driver/gpio.h"

void buzzer(void);
void setBuzzer(bool estado);