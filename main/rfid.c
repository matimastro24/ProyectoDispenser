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

		// Notificar a la máquina de estados
        state_machine_rfid_detected(uid_str);

		//feedback sonoro
		
		setBuzzer(true);
		vTaskDelay(pdMS_TO_TICKS(1000));
		setBuzzer(false);
		//http_get_uid_async(uid_str); no esta presensete para la maquina de estados
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