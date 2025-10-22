// lcd_driver.h
#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Configuración I2C
#define I2C_MASTER_SCL_IO 21       /*!< GPIO number used for I2C master clock 16 */ 
#define I2C_MASTER_SDA_IO 22       /*!< GPIO number used for I2C master data 17 */ 
#define I2C_MASTER_NUM I2C_NUM_0                   /*!< I2C port number for master dev */
#define I2C_PCF8574_FREQ_HZ 50000 /*!< I2C master clock frequency */
#define I2C_MASTER_TIMEOUT_MS 1000
#define PCF8574_ADDR 0x27        /*!< Address of the pcf8574 */


// Configuración LCD
#define LCD_COLS 20
#define LCD_ROWS 4

// Comandos del HD44780
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// Banderas de Entry Mode
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTDECREMENT 0x00

// Banderas de Display Control
#define LCD_DISPLAYON 0x04
#define LCD_DISPLAYOFF 0x00
#define LCD_CURSORON 0x02
#define LCD_CURSOROFF 0x00
#define LCD_BLINKON 0x01
#define LCD_BLINKOFF 0x00

// Banderas de Cursor Shift
#define LCD_DISPLAYMOVE 0x08
#define LCD_MOVERIGHT 0x04
#define LCD_MOVELEFT 0x00

// Banderas de Function Set
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_5x8DOTS 0x00

// Pines del PCF8574 (mapeados en bits)
#define PCF_RS 0  // Bit 0
#define PCF_RW 1  // Bit 1
#define PCF_E  2  // Bit 2
#define PCF_BL 3  // Bit 3 - Backlight



typedef struct {
    i2c_master_dev_handle_t dev_handle;
    uint8_t cols;
    uint8_t rows;
    uint8_t display_control;
    uint8_t entry_mode;
    uint8_t backlight_state;
    uint8_t last_data;  // Para mantener estado del PCF8574
} lcd_t;

// Funciones públicas
void i2c_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle);
void lcd_init(lcd_t *lcd, i2c_master_dev_handle_t dev_handle);
void lcd_write_char(lcd_t *lcd, char c);
void lcd_write_string(lcd_t *lcd, const char *str);
void lcd_printf(lcd_t *lcd, const char *fmt, ...);
void lcd_clear(lcd_t *lcd);
void lcd_home(lcd_t *lcd);
void lcd_set_cursor(lcd_t *lcd, uint8_t row, uint8_t col);
void lcd_cursor_on(lcd_t *lcd);
void lcd_cursor_off(lcd_t *lcd);
void lcd_blink_on(lcd_t *lcd);
void lcd_blink_off(lcd_t *lcd);
void lcd_display_on(lcd_t *lcd);
void lcd_display_off(lcd_t *lcd);
void lcd_shift_left(lcd_t *lcd);
void lcd_shift_right(lcd_t *lcd);
void lcd_backlight_on(lcd_t *lcd);
void lcd_backlight_off(lcd_t *lcd);
void lcd_create_char(lcd_t *lcd, uint8_t location, uint8_t charmap[8]);
void lcd_write_custom_char(lcd_t *lcd, uint8_t location);

#endif // LCD_DRIVER_H