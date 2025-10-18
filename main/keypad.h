#pragma once
#include <stddef.h>
#include "esp_err.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "buzzer.h"

void cols_high_impedance_startup_safe(void);
void set_all_cols_high(void);
void drive_only_col_low(int col);
void keypad_init(void);
bool keypad_scan_once(char *out_key, TickType_t debounce_ms);