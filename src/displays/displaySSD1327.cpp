#include "../core/options.h"
#if DSP_MODEL==DSP_SSD1327
#include "dspcore.h"
#include "../core/config.h"
#include "../core/logging.h"

#ifndef SCREEN_ADDRESS
  #define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; or scan it https://create.arduino.cc/projecthub/abdularbi17/how-to-scan-i2c-address-in-arduino-eaadda
#endif

// ---------------------------------------------------------------------------
// Panel-local grayscale palette (4-bit / 16 levels GS0..GS15).
// Defined before the interface switch so both the I2C and SPI branches, and
// initCommonTheme() below, can see them.
// ---------------------------------------------------------------------------
#define CLR_ITEM1    0xA
#define CLR_ITEM2    0x8
#define CLR_ITEM3    0x5
#define DARK_GRAY    0x01
#define SILVER       0x07
#define TFT_LOGO     0x3f
#define ORANGE       0x05

// ---------------------------------------------------------------------------
// Gray Scale Table (command 0xB8, 16 bytes for GS0..GS15).
//
// The table MUST be monotonic non-decreasing with table[0] == 0x00:
//   1. the 16 levels above stay visually distinct (playlist rows, VU bars);
//   2. the panel's hardware INVERSE DISPLAY command (0xA7, issued by
//      DspCore::invert()) works by REVERSING this mapping (GS n <-> GS 15-n).
//      A flat table therefore makes the whole screen flood with one colour.
//
// Do NOT flatten this table to gain brightness - raise SSD1327_CONTRAST /
// SSD1327_VCOMH below instead.
// ---------------------------------------------------------------------------
// Datasheet "standard linear" ramp (brighter monotonic option):
//   {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
//    0x08, 0x10, 0x18, 0x20, 0x2F, 0x38, 0x3F}
// Non-linear ramp used here, compensates panel non-uniformity:
static const uint8_t gamma_table[16] = {
  0x00, 0x01, 0x02, 0x03, 0x05, 0x07, 0x09, 0x0B,
  0x0D, 0x0F, 0x12, 0x15, 0x18, 0x1C, 0x20, 0x2A
};

// Brightness levers - override these in myoptions.h rather than editing the
// gray-scale table (the simplified revision flattened the table instead,
// which is what broke Invert Screen).
#ifndef SSD1327_CONTRAST
  #define SSD1327_CONTRAST  0xAA   // 0x81 arg, 0x00..0xFF (0xFF = max)
#endif
#ifndef SSD1327_VCOMH
  #define SSD1327_VCOMH     0x20   // 0xDB arg, 0x00..0xFF (0x40 = brighter)
#endif

// ---------------------------------------------------------------------------
// Theme palette - called from both interface branches, so it is defined above
// the interface switch (no forward declaration needed).
// ---------------------------------------------------------------------------
#if OLED_MONO
static void initCommonTheme() {
  #include "tools/oledcolorfix.h"
}
#else
static void initCommonTheme() {
  /* Ordered to match theme_t in core/config.h.
     Contract (importtheme.md / tools/oledcolorfix.h):
       ordinary : text = meta,   bar = metafill
       inverted : text = metabg, bar = metafill
     => meta   must be the BRIGHT value (ordinary text)
        metabg must be the DARK  value (ordinary bg / inverted text)
        metafill must equal meta, so the swap is a clean fg<->bg flip. */
  config.theme.background    = TFT_BG;      // 0x00
  config.theme.meta          = TFT_LOGO;    // 0x3f  [FIXED: bright = ordinary text]
  config.theme.metabg        = TFT_BG;      // 0x00  [FIXED: dark  = inverted text]
  config.theme.metafill      = TFT_LOGO;    // 0x3f
  config.theme.title1        = TFT_LOGO;
  config.theme.title2        = SILVER;
  config.theme.digit         = TFT_LOGO;
  config.theme.div           = DARK_GRAY;
  config.theme.weather       = ORANGE;
  config.theme.vupeak        = TFT_LOGO;
  config.theme.vumax         = TFT_LOGO;
  config.theme.vumin         = SILVER;
  config.theme.clock         = TFT_LOGO;
  config.theme.clockbg       = DARK_GRAY;
  config.theme.seconds       = SILVER;
  config.theme.dow           = SILVER;
  config.theme.date          = SILVER;
  config.theme.clockss       = TFT_LOGO;
  config.theme.clockbgss     = TFT_BG;
  config.theme.secondsss     = SILVER;
  config.theme.dowss         = SILVER;
  config.theme.datess        = SILVER;
  config.theme.buffer        = TFT_FG;
  config.theme.ip            = SILVER;
  config.theme.vol           = SILVER;
  config.theme.rssi          = TFT_FG;
  config.theme.battery       = TFT_FG;
  config.theme.bitrate       = TFT_LOGO;
  config.theme.volbarout     = TFT_FG;
  config.theme.volbarin      = SILVER;
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

  // Panel init. oled_command() is a protected member of Adafruit_SSD1327 and
  // cannot be called from a free helper, so this block is repeated verbatim in
  // the SPI branch below - keep the two copies identical.
  oled_command(0xB8);                                  // Set Gray Scale Table
  for (uint8_t i = 0; i < 16; i++) {
    oled_command(gamma_table[i]);                      // GS0..GS15, monotonic
  }
  oled_command(0x81); oled_command(SSD1327_CONTRAST);  // Set Contrast Current
  oled_command(0xDB); oled_command(SSD1327_VCOMH);     // Set VCOMH Deselect Level

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

  // Panel init - keep identical to the I2C branch above (see note there).
  oled_command(0xB8);                                  // Set Gray Scale Table
  for (uint8_t i = 0; i < 16; i++) {
    oled_command(gamma_table[i]);                      // GS0..GS15, monotonic
  }
  oled_command(0x81); oled_command(SSD1327_CONTRAST);  // Set Contrast Current
  oled_command(0xDB); oled_command(SSD1327_VCOMH);     // Set VCOMH Deselect Level

  initCommonTheme();
  cp437(true);
  flip();
  invert();
  setTextWrap(false);
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
