#include "../core/options.h"
#if DSP_MODEL==DSP_SSD1327
#include "dspcore.h"
#include "../core/config.h"
#include "../core/logging.h"

#ifndef SCREEN_ADDRESS
  #define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; or scan it https://create.arduino.cc/projecthub/abdularbi17/how-to-scan-i2c-address-in-arduino-eaadda
#endif

// Panel-local grayscale palette (4-bit / 16 levels). Defined before the interface
// switch so that both the I2C and SPI branches, and the theme function further
// down, can see them.
#define CLR_ITEM1    0xA
#define CLR_ITEM2    0x8
#define CLR_ITEM3    0x5
#define DARK_GRAY    0x01
#define SILVER       0x07
#define TFT_LOGO     0x3f
#define ORANGE       0x05

// Forward declaration
static void initCommonTheme();

// Auto-detect interface from pins
#if I2C_SDA!=255 && I2C_SCL!=255
#include <Wire.h>

#ifndef I2CFREQ_HZ
  #define I2CFREQ_HZ   6000000UL
#endif

TwoWire tw = TwoWire(0);

DspCore::DspCore(): Adafruit_SSD1327(DSP_WIDTH, DSP_HEIGHT, &tw, I2C_RST/*, I2CFREQ_HZ*/) {}

void DspCore::initDisplay() {
  tw.begin(I2C_SDA, I2C_SCL);
  if (!begin(SCREEN_ADDRESS)) {
    ERRORLOG("SSD1327 allocation failed");
    for (;;);
  }
  
  // --- Maximum brightness settings ---
  oled_command(0xB8);  // Set Gray Scale Table
  for (uint8_t i = 0; i < 16; i++) {
    oled_command(0x7F);  // 127 = max brightness for all levels
  }
  oled_command(0x81);  // Set Contrast Current
  oled_command(0xFF);  // 255 = maximum
  oled_command(0xDB);  // Set VCOMH Deselect Level
  oled_command(0x40);  // 64 = maximum
  // -----------------------------------
  
  initCommonTheme();
  cp437(true);
  flip();
  invert();
  setTextWrap(false);
}
#else
#ifndef DEF_SPI_FREQ
  #define DEF_SPI_FREQ        8000000UL      /*  set it to 0 for system default */
#endif

DspCore::DspCore(): Adafruit_SSD1327(DSP_WIDTH, DSP_HEIGHT, &SPI, TFT_DC, TFT_RST, TFT_CS, DEF_SPI_FREQ) {}

void DspCore::initDisplay() {
  if (!begin(SCREEN_ADDRESS)) {
    ERRORLOG("SSD1327 allocation failed");
    for (;;);
  }
  
  // --- Maximum brightness settings ---
  oled_command(0xB8);  // Set Gray Scale Table
  for (uint8_t i = 0; i < 16; i++) {
    oled_command(0x7F);  // 127 = max brightness for all levels
  }
  oled_command(0x81);  // Set Contrast Current
  oled_command(0xFF);  // 255 = maximum
  oled_command(0xDB);  // Set VCOMH Deselect Level
  oled_command(0x40);  // 64 = maximum
  // -----------------------------------
  
  initCommonTheme();
  cp437(true);
  flip();
  invert();
  setTextWrap(false);
}
#endif

#if OLED_MONO
static void initCommonTheme() {
  #include "tools/oledcolorfix.h"
}
#else
static void initCommonTheme() {
  /* Ordered to match theme_t in core/config.h */
  config.theme.background = TFT_BG;
  config.theme.meta       = TFT_LOGO;
  config.theme.metabg     = TFT_BG;
  config.theme.metafill   = TFT_LOGO;
  config.theme.title1     = TFT_LOGO;
  config.theme.title2     = SILVER;
  config.theme.digit      = TFT_LOGO;
  config.theme.div        = DARK_GRAY;
  config.theme.weather    = ORANGE;
  config.theme.vumax      = TFT_LOGO;   // brightest available level
  config.theme.vumin      = SILVER;     // dimmer level for the low end of the meter
  config.theme.clock      = TFT_LOGO;
  config.theme.clockbg    = DARK_GRAY;
  config.theme.seconds    = SILVER;
  config.theme.dow        = SILVER;
  config.theme.date       = SILVER;
  config.theme.clockss    = TFT_LOGO;
  config.theme.clockbgss  = TFT_BG;
  config.theme.secondsss  = SILVER;
  config.theme.dowss      = SILVER;
  config.theme.datess     = SILVER;
  config.theme.buffer     = TFT_FG;
  config.theme.ip         = SILVER;
  config.theme.vol        = SILVER;
  config.theme.rssi       = TFT_FG;
  config.theme.battery    = TFT_FG;
  config.theme.bitrate    = TFT_LOGO;
  config.theme.volbarout  = TFT_FG;
  config.theme.volbarin   = SILVER;
  config.theme.plcurrent     = TFT_BG;
  config.theme.plcurrentbg   = TFT_FG;
  config.theme.plcurrentfill = TFT_FG;
  config.theme.playlist[0] = CLR_ITEM1;
  config.theme.playlist[1] = CLR_ITEM2;
  config.theme.playlist[2] = CLR_ITEM3;
  config.theme.playlist[3] = CLR_ITEM3;
  config.theme.playlist[4] = CLR_ITEM3;
}
#endif

void DspCore::clearDsp(bool black){ fillScreen(black?0:config.theme.background); }
void DspCore::flip(){
#if DSP_WIDTH==DSP_HEIGHT
  if(ROTATE_90){
    setRotation(config.store.flipscreen?3:1);
  }else{
    setRotation(config.store.flipscreen?2:0);
  }
#else
  setRotation(config.store.flipscreen?2:0);
#endif
}
void DspCore::invert(){ invertDisplay(config.displayIsInverted != DSP_INVERT_QUIRK); }
void DspCore::sleep(void){ oled_command(SSD1327_DISPLAYOFF); }
void DspCore::wake(void){ oled_command(SSD1327_DISPLAYON); }

#endif