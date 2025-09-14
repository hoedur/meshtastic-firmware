// #define BUTTON_PIN 2
// #define BUTTON_NEED_PULLUP

#define PIN_BUZZER 11
#define HAS_NEOPIXEL                         // Enable the use of neopixels
#define NEOPIXEL_COUNT 1                     // How many neopixels are connected
#define NEOPIXEL_DATA 2                      // gpio pin used to send data to the neopixels
#define NEOPIXEL_TYPE (NEO_GRB + NEO_KHZ800) // type of neopixels in use
#define ENABLE_AMBIENTLIGHTING               // Turn on Ambient Lighting
#define SPI_MISO_GPIO 22
#define SPI_SCLK_GPIO 20
#define SPI_MOSI_GPIO 21

#define GROVE_TX_PIN 5
#define GROVE_RX_PIN 4

#undef LORA_SCK
#undef LORA_MISO
#undef LORA_MOSI
#undef LORA_CS

#define LORA_SCK SPI_SCLK_GPIO
#define LORA_MISO SPI_MISO_GPIO
#define LORA_MOSI SPI_MOSI_GPIO
#define LORA_CS 23
// LoRa SX1262
#define USE_SX1262
#define SX126X_MAX_POWER 22
#define SX126X_BUSY 19
#define SX126X_DIO1 7
#define SX126X_RESET -1
#define SX126X_CS LORA_CS
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_RXEN RADIOLIB_NC
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 3.0

#define USE_SPISSD1306
#ifdef USE_SPISSD1306
#define SSD1306_NSS 6 // CS
#define SSD1306_RS 18 // DC
#define SSD1306_RESET 15
// #define OLED_DG 1
#endif
#define SCREEN_TRANSITION_FRAMERATE 10
#define BRIGHTNESS_DEFAULT 130 // Medium Low Brightness