// lcd_driver.c
#include "lcd_driver.h"
#include "esp_log.h"
#include "unistd.h"
#include "stdio.h"
#include "stdarg.h"
#include "string.h"

static const char *TAG = "LCD_DRIVER";
// Funciones privadas
static void pcf_write_byte(lcd_t *lcd, uint8_t data);
static void pcf_send_pulse(lcd_t *lcd, uint8_t data);
static void write_nibble(lcd_t *lcd, uint8_t nibble, uint8_t rs);
static void write_command(lcd_t *lcd, uint8_t cmd);
static void write_data(lcd_t *lcd, uint8_t data);
static void wait_busy(void);

// Espera de seguridad
static void wait_busy(void) {
    usleep(500);
}

// Escribe un byte al PCF8574.
static void pcf_write_byte(lcd_t *lcd, uint8_t data) {
    lcd->last_data = data;
    esp_err_t ret = i2c_master_transmit(lcd->dev_handle, &data, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error escribiendo al PCF8574: %s", esp_err_to_name(ret));
    }
}

// Envía pulso de enable.
static void pcf_send_pulse(lcd_t *lcd, uint8_t data) {
    pcf_write_byte(lcd, data | (1 << PCF_E));  // E = 1
    usleep(10);
    pcf_write_byte(lcd, data & ~(1 << PCF_E)); // E = 0
    usleep(10);
}

// Escribe un nibble (4 bits), sin modificar el estado de la backlight.
static void write_nibble(lcd_t *lcd, uint8_t nibble, uint8_t rs) {
    uint8_t data = (nibble << 4);  // Los 4 bits en D7:D4
    
    if (rs) {
        data |= (1 << PCF_RS);  // RS = 1 para datos
    } else {
        data &= ~(1 << PCF_RS); // RS = 0 para comandos
    }
    
    data |= (lcd->backlight_state << PCF_BL);
    pcf_send_pulse(lcd, data);
}

// Escribe un comando completo (2 nibbles en 4-bit)
static void write_command(lcd_t *lcd, uint8_t cmd) {
    uint8_t high = (cmd >> 4) & 0x0F;
    uint8_t low = cmd & 0x0F;
    
    write_nibble(lcd, high, 0);  // RS = 0 (comando)
    usleep(100);
    write_nibble(lcd, low, 0);   // RS = 0 (comando)
    usleep(100);
    //en lugar de leer la busy flag de la lcd, simplemente se espera un timepo fijo.
    wait_busy();
}

// Escribe datos (carácter)
static void write_data(lcd_t *lcd, uint8_t data) {
    uint8_t high = (data >> 4) & 0x0F;
    uint8_t low = data & 0x0F;
    
    write_nibble(lcd, high, 1);  // RS = 1 (dato)
    usleep(100);
    write_nibble(lcd, low, 1);   // RS = 1 (dato)
    usleep(100);
    wait_busy();
}

//Inicilización de la comunicacion I2C.
void i2c_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_LOGI(TAG, "Iniciando cominacion I2C con LCD.");
    esp_err_t ret = i2c_new_master_bus(&bus_config, bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando maestro I2C: %s", esp_err_to_name(ret));
    }
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8574_ADDR,
        .scl_speed_hz = I2C_PCF8574_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando dispositivo I2C: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Comunicacion I2C con LCD iniciada correctamente.");
}

// Inicializa el LCD en modo 4-bit
void lcd_init(lcd_t *lcd, i2c_master_dev_handle_t dev_handle) {
    lcd->dev_handle = dev_handle;
    lcd->cols = LCD_COLS;
    lcd->rows = LCD_ROWS;
    lcd->backlight_state = 1;
    lcd->display_control = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
    lcd->entry_mode = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
    lcd->last_data = 0;

    ESP_LOGI(TAG, "Inicializando LCD 20x4 en modo 4-bit");
    
    usleep(50000);  // Espera inicial post-power
    
    // Secuencia de inicialización en 4-bit (HD44780 datasheet)
    // Primero fuerza 8-bit mode 3 veces
    write_nibble(lcd, 0x03, 0);
    usleep(4100);
    
    write_nibble(lcd, 0x03, 0);
    usleep(100);
    
    write_nibble(lcd, 0x03, 0);
    usleep(100);
    
    // Ahora cambia a 4-bit mode
    write_nibble(lcd, 0x02, 0);
    usleep(100);
    
    // Ahora que está en 4-bit, envía comandos normales
    write_command(lcd, LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS);
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
    lcd_clear(lcd);
    write_command(lcd, LCD_ENTRYMODESET | lcd->entry_mode);
    
    ESP_LOGI(TAG, "LCD inicializado correctamente");
}


// Limpia la pantalla
void lcd_clear(lcd_t *lcd) {
    write_command(lcd, LCD_CLEARDISPLAY);
    usleep(2000);
}

// Cursor a home (0,0)
void lcd_home(lcd_t *lcd) {
    write_command(lcd, LCD_RETURNHOME);
    usleep(2000);
}

// Posiciona el cursor en fila y columna
void lcd_set_cursor(lcd_t *lcd, uint8_t row, uint8_t col) {
    uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    
    if (row >= lcd->rows) row = 0;
    if (col >= lcd->cols) col = 0;
    
    write_command(lcd, LCD_SETDDRAMADDR | (row_offsets[row] + col));
}

// Enciende el cursor
void lcd_cursor_on(lcd_t *lcd) {
    lcd->display_control |= LCD_CURSORON;
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
}

// Apaga el cursor
void lcd_cursor_off(lcd_t *lcd) {
    lcd->display_control &= ~LCD_CURSORON;
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
}

// Enciende parpadeo
void lcd_blink_on(lcd_t *lcd) {
    lcd->display_control |= LCD_BLINKON;
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
}

// Apaga parpadeo
void lcd_blink_off(lcd_t *lcd) {
    lcd->display_control &= ~LCD_BLINKON;
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
}

// Enciende pantalla
void lcd_display_on(lcd_t *lcd) {
    lcd->display_control |= LCD_DISPLAYON;
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
}

// Apaga pantalla
void lcd_display_off(lcd_t *lcd) {
    lcd->display_control &= ~LCD_DISPLAYON;
    write_command(lcd, LCD_DISPLAYCONTROL | lcd->display_control);
}

// Desplaza el contenido a la izquierda
void lcd_shift_left(lcd_t *lcd) {
    write_command(lcd, LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVELEFT);
}

// Desplaza el contenido a la derecha
void lcd_shift_right(lcd_t *lcd) {
    write_command(lcd, LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVERIGHT);
}

// Enciende backlight
void lcd_backlight_on(lcd_t *lcd) {
    lcd->backlight_state = 1;
    uint8_t data = (1 << PCF_BL);
    pcf_write_byte(lcd, data);
}

// Apaga backlight
void lcd_backlight_off(lcd_t *lcd) {
    lcd->backlight_state = 0;
    uint8_t data = 0;
    pcf_write_byte(lcd, data);
}

// Escribe un carácter
void lcd_write_char(lcd_t *lcd, char c) {
    write_data(lcd, (uint8_t)c);
}

// Escribe una cadena
void lcd_write_string(lcd_t *lcd, const char *str) {
    while (*str) {
        lcd_write_char(lcd, *str++);
    }
}

// Escribe con formato (printf)
void lcd_printf(lcd_t *lcd, const char *fmt, ...) {
    char buf[LCD_COLS + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    lcd_write_string(lcd, buf);
}

// Define un carácter personalizado
void lcd_create_char(lcd_t *lcd, uint8_t location, uint8_t charmap[8]) {
    location &= 0x07;  // Solo 8 caracteres personalizados (0-7)
    write_command(lcd, LCD_SETCGRAMADDR | (location << 3));
    
    for (int i = 0; i < 8; i++) {
        write_data(lcd, charmap[i]);
    }
}

// Escribe un carácter personalizado previamente definido
void lcd_write_custom_char(lcd_t *lcd, uint8_t location) {
    location &= 0x07;
    lcd_write_char(lcd, location);
}

/*escribe con saltos de linea automaticos*/
void lcd_write_auto(lcd_t *lcd, const char *str, uint8_t start_row, uint8_t start_col) {
    uint8_t row = start_row;
    uint8_t col = start_col;

    // Recorremos cada carácter de la cadena
    for (size_t i = 0; i < strlen(str); i++) {
        // Si llegamos al final de la columna, pasamos al siguiente renglón
        if (col >= LCD_COLS) {
            col = 0;
            row++;
        }

        // Si llegamos más allá de la última fila, dejamos de escribir
        if (row >= LCD_ROWS) {
            break;
        }

        // Posicionamos el cursor y escribimos el carácter
        lcd_set_cursor(lcd, row, col);
        char c[2] = { str[i], '\0' }; // Convertimos el carácter en string
        lcd_write_string(lcd, c);

        col++;
    }
}
