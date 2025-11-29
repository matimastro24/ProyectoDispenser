#include "rfid.h"

static const char *TAG = "RFID";


static void on_picc_state_changed(void *arg, esp_event_base_t base,
								  int32_t event_id, void *data) {
	rc522_picc_state_changed_event_t *event =
		(rc522_picc_state_changed_event_t *)data;
	rc522_picc_t *picc = event->picc;

	if (picc->state == RC522_PICC_STATE_ACTIVE) {
		char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX];
		rc522_picc_uid_to_str(&picc->uid, uid_str, sizeof(uid_str));
		ESP_LOGI(TAG, "RFID UID: %s", uid_str);

        // Verificamos que sea una tarjeta de 4 bytes (Mifare Classic estándar)
        uint32_t rfid_num = 0;
        
        if (picc->uid.length == 4) {
            // Convertimos el array de 4 bytes [0x19, 0x9D...] a un entero 0x199D...
            rfid_num = ((uint32_t)picc->uid.value[0] << 24) |
                       ((uint32_t)picc->uid.value[1] << 16) |
                       ((uint32_t)picc->uid.value[2] << 8)  |
                       (uint32_t)picc->uid.value[3];
            
            ESP_LOGI(TAG, "RFID Numérico: %" PRIu32 " (Hex: %X)", rfid_num, rfid_num);
        } else {
            ESP_LOGW(TAG, "Longitud de UID no soportada (%d bytes)", picc->uid.length);
        }

		// Notificar a la máquina de estados
        state_machine_rfid_detected(rfid_num);

		//feedback sonoro
		xTaskCreate(buzzer_task, "buzzer_task", 2048, NULL, 5, NULL);
	} else if (picc->state == RC522_PICC_STATE_IDLE &&
			   event->old_state >= RC522_PICC_STATE_ACTIVE) {
		ESP_LOGI(TAG, "Card has been removed");
	}
}

void rfid_init(rc522_driver_handle_t *driver, rc522_handle_t *scanner) {
	rc522_spi_config_t driver_config = {
		.host_id = SPI3_HOST,
		.bus_config =
			&(spi_bus_config_t){
				.miso_io_num = RC522_SPI_BUS_GPIO_MISO,
				.mosi_io_num = RC522_SPI_BUS_GPIO_MOSI,
				.sclk_io_num = RC522_SPI_BUS_GPIO_SCLK,
			},
		.dev_config =
			{
				.spics_io_num = RC522_SPI_SCANNER_GPIO_SDA,
			},
		.rst_io_num = RC522_SCANNER_GPIO_RST,
	};

    rc522_spi_create(&driver_config, driver);
    rc522_driver_install(*driver);

    rc522_config_t scanner_config = {
        .driver = *driver,
    };

    rc522_create(&scanner_config, scanner);
    rc522_register_events(*scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
}