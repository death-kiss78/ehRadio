#ifndef myoptions_h
#define myoptions_h

/*        ************************************************************************      */
/*        *        This file must be in the root folder of the sketch !!!        *      */
/*        ************************************************************************      */
/*        . . .  CHECK options.h for full options, examples, and overrides   . . .      */

// ESP32-S3-DevKitC-1 N16R8 (16MB Flash 8MB PSRAM)
// Display: ILI9488 (SPI TFT)
// Audio Decoder: I2S (PCM I2S Decoder)
// SPI Bus A: ILI9488 (SPI TFT)
// SPI Bus B: Touch Screen: XPT2046 (SPI)
// SPI Bus A: SD Card Reader
//
//                  |----------|
//              3V3 |          | G
//              3V3 |          | 43
//               -1 |          | 44
// BRIGHTNESS_PIN 4 |          | 1 TS_CS
//         IR_PIN 5 |          | 2 SPIB_MOSI
//          SD_CS 6 |          | 42 SPIB_SCK
//       I2S_DOUT 7 |          | 41 SPIB_MISO
//      I2S_BCLK 15 |          | 40
//       I2S_LRC 16 |          | 39
//               17 |          | 38
//               18 |          | 37
//                8 |          | 36
//       SPIA_SCK 3 |          | 35
//     SPIA_MISO 46 |          | 0 BTN_MODE
//                9 |          | 45 SPIA_MOSI
//               10 |          | 48 RGB_LED_PIN
//               11 |          | 47 TFT_DC
//               12 |          | 21 TFT_RST
//               13 |          | 20
//        TFT_CS 14 |          | 19
//               5V |          | G
//                G |          | G
//                  |----------|


/* --- Firmware File --- */
#define FIRMWARE_NAME        "av3ntura" /* your ehRadio's name */

/* --- SPI Bus Pins --- */
#define SPIA_SCK             3
#define SPIA_MISO            46
#define SPIA_MOSI            45
#define SPIB_SCK             42
#define SPIB_MISO            41
#define SPIB_MOSI            2

/* --- Display --- */
#define DSP_MODEL            DSP_ILI9488
#define TFT_DC               47
#define DSP_WIDTH            480       /* override default width (480) */
#define DSP_HEIGHT           320       /* override default height (320) */
#define TFT_RST              21        /* pin RST is attached to (-1 = EN pin) */
#define BRIGHTNESS_PIN       4         /* pin that controls brightness / backlight (255 = unused) */
#define DSP_DIMMING_ENABLED  true      /* enable screen dimming (depends on brightness pin) */
#define DSP_INVERT_QUIRK     false     /* fixes display inversion quirk (very common) */
#define BIG_BOOT_LOGO        true      /* set to false to save space */
#define TFT_CS               14        /* pin CS is attached to (255 = tied to GND) */

/* --- Audio Decoder --- */
#define I2S_DOUT             7
#define I2S_BCLK             15
#define I2S_LRC              16

/* --- Inputs --- */
#define TS_MODEL             TS_MODEL_XPT2046
#define TS_CS                1
#define TS_SPI               'B'       /* assign touchscreen to SPI Bus */
#define BTN_MODE             0
#define BTN_MODE_PULLUP      true      /* use internal pullup on button (default true) */

/* --- Peripherals and Build Options --- */
#define DISPLAYFONT          MATRIXCHUNKY /* MATRIXCHUNKY (default), MATRIXLIGHT, X11 */
#define CLOCKFONT            CHUNKY6   /* CHUNKY6 (OLED default), CHUNKY6_PX (TFT default), YO_MONO (7-segment/display font) */
#define RGB_LED_PIN          48
#define IR_PIN               5
#define IR_TIMEOUT           80        /* kTimeout, see IRremoteESP8266 documentation */
#define SD_CS                6
#define SD_SPI               'A'       /* assign SD to SPI bus */
#define SD_CARD_DETECT_PIN   255       /* Pin DETECT is attached to (255 for unused) */
#define SD_AUTOPLAY          false     /* auto-switch to SD card mode when card is inserted (requires SD_CARD_DETECT_PIN != 255) */

/* --- User Defaults --- */
#define DSP_LOCALE           "ro_RO"
#define TIMEZONE_NAME        "Europe/Rome"
#define TIMEZONE_POSIX       "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:0"
#define CLOCK_TWELVE         false
#define NUMBERED_PLAYLIST    false
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

/* --- Extra defines --- */
#define SDMMC_CLK   39
#define SDMMC_CMD   38
#define SDMMC_D0    40
#define SDMMC_FREQ 0 // in kHz: 0 = driver default (BOARD_MAX_SDMMC_FREQ = SDMMC_FREQ_HIGHSPEED, 40MHz); set e.g. 20000 for 20MHz if a card is flaky

/* --- Battery --- */
#define BATTERY_PIN 9                  /* GPIO9: ADC pin for battery voltage */
#define BATTERY_DIVIDER_RATIO 2.0      /* 100k + 100k voltage divider = 1:2 ratio */
#define BATTERY_ADC_REF_MV    3438     /* ESP32-S3 ADC reference voltage (calibrated EL103565 3000mAh 11.1Wh) */
#define BATTERY_UPDATE_INTERVAL 60     /* Update every 60 seconds */

#endif // myoptions_h
