#ifndef RFID_H
#define RFID_H

#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"
#include "buzzer.h"
#include "http_Server.h"
#include <esp_log.h>
#include "state_machine.h"

#define RC522_SPI_BUS_GPIO_MISO    (19)
#define RC522_SPI_BUS_GPIO_MOSI    (23)
#define RC522_SPI_BUS_GPIO_SCLK    (18)
#define RC522_SPI_SCANNER_GPIO_SDA (5)
#define RC522_SCANNER_GPIO_RST     (-1) // soft-reset

void rfid_init(rc522_driver_handle_t *driver, rc522_handle_t *scanner);
#endif