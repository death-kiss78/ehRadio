#ifndef myoptions_h
#define myoptions_h

/*        ************************************************************************      */
/*        *        This file must be in the root folder of the sketch !!!        *      */
/*        ************************************************************************      */
/*        . . .  CHECK options.h for full options, examples, and overrides   . . .      */

// ESP32-S3-DevKitC-1 N16R8 (16MB Flash 8MB PSRAM)
// Display: ILI9341 (SPI 320x240 TFT)
// Audio Decoder: I2S (PCM I2S Decoder)
// SPI Bus A: ILI9341 (SPI 320x240 TFT)
// SPI Bus B: SD Card Reader
//
//              |----------|
//          3V3 |          | G
//          3V3 |          | 43
//           -1 |          | 44
//            4 |          | 1 IR_PIN
//            5 |          | 2 SD_CS
//            6 |          | 42
//   I2S_DOUT 7 |          | 41
//  I2S_BCLK 15 |          | 40 SPIB_MOSI
//   I2S_LRC 16 |          | 39 SPIB_MISO
//           17 |          | 38 SPIB_SCK
//           18 |          | 37
//            8 |          | 36
//   SPIA_SCK 3 |          | 35
// SPIA_MISO 46 |          | 0 BTN_MODE
//            9 |          | 45 SPIA_MOSI
//           10 |          | 48 RGB_LED_PIN
//           11 |          | 47 TFT_DC
//           12 |          | 21 TFT_RST
//           13 |          | 20
//    TFT_CS 14 |          | 19
//           5V |          | G
//            G |          | G
//              |----------|


/* --- Firmware File --- */
#define FIRMWARE_NAME        "av3ntura" /* your ehRadio's name */
#define MQTT_ENABLE false

/* --- SPI Bus Pins --- */
#define SPIA_SCK             3
#define SPIA_MISO            46
#define SPIA_MOSI            45
#define SPIB_SCK             38
#define SPIB_MISO            39
#define SPIB_MOSI            40

/* --- Display --- */
#define DSP_MODEL            DSP_ILI9341
#define TFT_DC               47
#define TFT_RST              21        /* pin RST is attached to (-1 = EN pin) */
#define BRIGHTNESS_PIN       255       /* pin that controls brightness / backlight (255 = unused) */
#define TFT_CS               14        /* pin CS is attached to (255 = tied to GND) */

/* --- Audio Decoder --- */
#define I2S_DOUT             7
#define I2S_BCLK             15
#define I2S_LRC              16

/* --- Inputs --- */
#define BTN_MODE             0
#define BTN_MODE_PULLUP      true     /* use internal pullup on button (default true) */

/* --- Peripherals and Build Options --- */
#define DISPLAYFONT          MATRIXCHUNKY /* MATRIXCHUNKY (default), MATRIXLIGHT, X11 */
#define CLOCKFONT            LED /* CHUNKY6 (OLED default), CHUNKY6_PX (TFT default), YO_MONO (7-segment/display font) */
#define RGB_LED_PIN          48
#define IR_PIN               1
#define IR_TIMEOUT           80        /* kTimeout, see IRremoteESP8266 documentation */
//#define SD_SPI               'M'       /* assign SD to SPI bus */

/* --- User Defaults --- */
#define DSP_LOCALE           "ro_RO"
#define TIMEZONE_NAME        "Europe/Rome"
#define TIMEZONE_POSIX       "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:00"
#define CLOCK_TWELVE         false
#define NUMBERED_PLAYLIST    true
#define ONE_CLICK_SWITCH     false
#define SCREEN_FLIP          false
#define SS_PLAYING           false
#define SS_NOTPLAYING        false
#define SD_SHUFFLE           false
#define SHOW_BUFFERBAR       true
#define SHOW_VU_METER        true
#define SMART_START          false
#define SNTP_1               "time.windows.com"
#define SNTP_2               ""
#define TOUCH_FLIP           "false"
#define VOLUME_PAGE          false
#define WEATHER_METRIC       true
#define WEATHER_LAT          "37.94635" /* latitude */
#define WEATHER_LON          "15.36671" /* longitude */
#define WIFI_SCAN_BEST_RSSI  true



#define SDMMC_CLK   39
#define SDMMC_CMD   38
#define SDMMC_D0    40
#define SDMMC_FREQ 0 // in kHz: 0 = driver default (BOARD_MAX_SDMMC_FREQ = SDMMC_FREQ_HIGHSPEED, 40MHz); set e.g. 20000 for 20MHz if a card is flaky



/* --- Extra defines --- */
/*

// ===================== SD CARD =====================
// Lafvin ESP32-S3-CAM use SDMMC on 1-bit
#define CONFIG_SOC_SDMMC_USE_GPIO_MATRIX 1
#define CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4 0   // only 1-bit

// Pins SDMMC LAFVIN ESP32-S3-CAM
#define CONFIG_EXAMPLE_PIN_CLK  GPIO_NUM_39   // SD_CLK
#define CONFIG_EXAMPLE_PIN_CMD  GPIO_NUM_38   // SD_CMD
#define CONFIG_EXAMPLE_PIN_D0   GPIO_NUM_40   // SD_DATA0


 --- SD Card (SDMMC hardware bus — NOT SPI) --- -----------freenove 2.8
 SDMMC uses dedicated pins:
   CLK = GPIO41
   CMD = GPIO40
   D0  = GPIO42
   D1  = GPIO39
   D2  = GPIO38
   D3  = GPIO37
   These are handled automatically by ESP-IDF and ehRadio.

*/

#endif // myoptions_h
