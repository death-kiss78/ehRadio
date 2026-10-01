#ifndef myoptions_h
#define myoptions_h

/*        ************************************************************************      */
/*        *        This file must be in the root folder of the sketch !!!        *      */
/*        ************************************************************************      */
/*        . . .  CHECK options.h for full options, examples, and overrides   . . .      */

// ESP32-S3 ES3C28P (ESP32-S3-N16R8)
// Display: ILI9341 (SPI 320x240 TFT)
// Audio Decoder: ES8311 (PCM I2S Mono Decoder)
// SPI Bus A: ILI9341 (SPI 320x240 TFT)
// SPI Bus B: SD Card Reader
//
//  Pin  Function
//  ---  --------
//  -1   TFT_RST
//  1    MUTE_PIN
//  2    BTN_MODE
//  3    IR_PIN
//  4    I2S_MCLK
//  5    I2S_BCLK
//  6    I2S_DIN
//  7    I2S_LRC
//  8    I2S_DOUT
//  10   TFT_CS
//  11   SPIA_MOSI
//  12   SPIA_SCK
//  15   ES8311_I2C_SCL + TS_SCL
//  16   ES8311_I2C_SDA + TS_SDA
//  17   TS_INT
//  18   TS_RST
//  42   RGB_LED_PIN
//  45   BRIGHTNESS_PIN
//  46   TFT_DC
//  47   SD_CS


/* --- Firmware File --- */
#define FIRMWARE_NAME        "freenove" /* your ehRadio's name */

/* --- SPI Bus Pins --- */
#define SPIA_SCK             12
#define SPIA_MISO            13
#define SPIA_MOSI            11
#define SPIB_SCK             38
#define SPIB_MISO            39
#define SPIB_MOSI            40

/* --- Display --- */
#define DSP_MODEL            DSP_ILI9341
#define TFT_DC               46
#define TFT_RST              -1        /* pin RST is attached to (-1 = EN pin) */
#define BRIGHTNESS_PIN       45        /* pin that controls brightness / backlight (255 = unused) */
#define DSP_DIMMING_ENABLED  true      /* enable screen dimming (depends on brightness pin) */
#define DSP_INVERT_QUIRK     true      /* fixes display inversion quirk (very common) */
#define TFT_CS               10        /* pin CS is attached to (255 = tied to GND) */

/* --- Audio Decoder --- */
#define I2S_MCLK             4
#define I2S_BCLK             5
#define I2S_LRC              7
#define I2S_DOUT             8
#define I2S_DIN              6
#define MUTE_PIN             1         /* pin MUTE is attached to (255 for unused) */
#define ES8311_I2C_SDA       16        /* may fix volume control on boot */
#define ES8311_I2C_SCL       15        /* may fix volume control on boot */
#define USE_ES8311
#define MUTE_VAL             HIGH      /* enables turning off audio amplifier */
#define ES8311_MAX_I2S       180       /* maximum I2S value to allow when mapping to ES8311 codec (0..254) */
#define PLAYER_FORCE_MONO    true      /* forces VU meter to mono mode */

/* --- Inputs --- */
#define TS_MODEL             TS_MODEL_FT6336
#define TS_SDA               16
#define TS_SCL               15
#define TS_INT               17
#define TS_RST               18
#define BTN_MODE             0

/* --- Peripherals and Build Options --- */
#define RGB_LED_PIN          42
#define IR_PIN               3
#define IR_TIMEOUT           80        /* kTimeout, see IRremoteESP8266 documentation */
#define SD_CS                47
#define SD_SPI               'B'       /* assign SD to SPI bus */

/* --- User Defaults --- */
#define DSP_LOCALE           "ro_RO"
#define TIMEZONE_NAME        "Europe/Rome"
#define TIMEZONE_POSIX       "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:0"
#define NUMBERED_PLAYLIST    true
#define ONE_CLICK_SWITCH     false
#define SCREEN_FLIP          true
#define SS_PLAYING           true
#define SS_NOTPLAYING        true
#define SHOW_BUFFERBAR       true
#define SHOW_VU_METER        true
#define SMART_START          false
#define SNTP_1               "time.windows.com"
#define SNTP_2               ""
#define TOUCH_FLIP           false
#define VOLUME_PAGE          false
#define WEATHER_METRIC       true
#define WEATHER_LAT          "37.94635" /* latitude */
#define WEATHER_LON          "15.36671" /* longitude */
#define WIFI_SCAN_BEST_RSSI  true

/* --- Extra defines --- */

/*
#define SDMMC_CLK 38
#define SDMMC_CMD 40
#define SDMMC_D0 39
#define SDMMC_D1 41
#define SDMMC_D2 48
#define SDMMC_D3 47
*/

/* --- Battery --- */
#define BATTERY_PIN 9                  /* GPIO9: ADC pin for battery voltage */
#define BATTERY_DIVIDER_RATIO 2.0      /* 100k + 100k voltage divider = 1:2 ratio */
#define BATTERY_ADC_REF_MV    3438     /* ESP32-S3 ADC reference voltage (calibrated EL103565 3000mAh 11.1Wh) */
#define BATTERY_UPDATE_INTERVAL 60     /* Update every 60 seconds */



#endif // myoptions_h
