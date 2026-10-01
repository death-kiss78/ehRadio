#ifndef ehRadio_themes_h
#define ehRadio_themes_h

#include <stdint.h>

// Theme Designer: https://vip-cxema.org/index.php/online-kalkulyatory/yoradio-redaktor-tem
// Can mostly be used to feed the importtheme.py script but the clock screensaver colors
// will need manual adjustment: clockss, clockbgss, secondsss, dowss, datess

struct ThemeData {
  uint16_t background;
  uint16_t meta;
  uint16_t metabg;
  uint16_t metafill;
  uint16_t title1;
  uint16_t title2;
  uint16_t digit;
  uint16_t div;
  uint16_t line;
  uint16_t weather;
  uint16_t vuaxis;
  uint16_t vupeak;
  uint16_t vumax;
  uint16_t vumin;
  uint16_t clock;
  uint16_t clockbg;
  uint16_t seconds;
  uint16_t dow;
  uint16_t date;
  uint16_t clockss;
  uint16_t clockbgss;
  uint16_t secondsss;
  uint16_t dowss;
  uint16_t datess;
  uint16_t buffer;
  uint16_t ip;
  uint16_t vol;
  uint16_t rssi;
  uint16_t battery;
  uint16_t bitrate;
  uint16_t volbarout;
  uint16_t volbarin;
  uint16_t plcurrent;
  uint16_t plcurrentbg;
  uint16_t plcurrentfill;
  uint16_t playlist[5];
};

#define RGB(r, g, b) ((uint16_t)(((r) >> 3) << 11) | ((uint16_t)((g) >> 2) << 5) | ((uint16_t)(b) >> 3))

const char _themeNames[][64] PROGMEM = {
    "ehRadio Blue & Red",
    "Colorful I",
    "vip-cxema.org",
};

const ThemeData _themes[] PROGMEM = {
    {   // ehRadio Blue & Red (Trip5)
        .background   = RGB(  0,   0,   0),
        .meta         = RGB(247, 247, 247),
        .metabg       = RGB(  0,  63, 207),
        .metafill     = RGB(  0,  55, 191),
        .title1       = RGB(239, 239, 239),
        .title2       = RGB(207, 207, 207),
        .digit        = RGB(255,  31,   7),
        .div          = RGB( 91,  91,  91),
        .line         = RGB( 91,  91,  91),
        .weather      = RGB(223, 223,   0),
        .vuaxis       = RGB( 22,  22,  22),
        .vupeak       = RGB(239, 239, 239),
        .vumax        = RGB(175,  31,  31),
        .vumin        = RGB( 15, 127,  15),
        .clock        = RGB(255,  31,   7),
        .clockbg      = RGB( 0,   0,   0),
        .seconds      = RGB(247,  27,   5),
        .dow          = RGB(255, 192, 192),
        .date         = RGB(192, 192, 255),
        .clockss      = RGB(153, 217, 234),
        .clockbgss    = RGB(  8,  11,  12),
        .secondsss    = RGB(140, 200, 220),
        .dowss        = RGB(110, 110, 150),
        .datess       = RGB(150, 110, 110),
        .buffer       = RGB(231,  47, 255),
        .ip           = RGB(153, 217, 234),
        .vol          = RGB(223, 223,   0),
        .rssi         = RGB(153, 217, 234),
        .battery      = RGB(153, 217, 234),
        .bitrate      = RGB(231,  47, 255),
        .volbarout    = RGB(223, 223,   0),
        .volbarin     = RGB(207, 207,   0),
        .plcurrent    = RGB(255, 255, 255),
        .plcurrentbg  = RGB(255,  31,   7),
        .plcurrentfill= RGB(231,  23,   7),
        .playlist     = {RGB(231,231,231), RGB(199,199,199), RGB(167,167,167), RGB(135,135,135), RGB(103,103,103)},
    },
    {   // Colorful I (Levente Daradici)
        .background    = RGB(  0,   0,   0),
        .meta          = RGB(255, 255, 255),
        .metabg        = RGB(  0,   0, 249),
        .metafill      = RGB(  0,   0, 249),
        .title1        = RGB(255, 255,   0),
        .title2        = RGB(255,   0, 255),
        .digit         = RGB(100, 100, 255),
        .div           = RGB(255, 255, 255),
        .line          = RGB(255, 255, 255),
        .weather       = RGB(  0, 255,   0),
        .vuaxis        = RGB( 63,  63,  63),
        .vupeak        = RGB(255, 255,   0),
        .vumax         = RGB(255,   0,   0),
        .vumin         = RGB(  0,   0, 255),
        .clock         = RGB(255, 255, 255),
        .clockbg       = RGB( 0,  0,  0),
        .seconds       = RGB(255, 255,   0),
        .dow           = RGB(255,   0, 202),
        .date          = RGB(108, 119, 255),
        .clockss       = RGB(127, 127, 127),
        .clockbgss     = RGB( 19,  19,  19),
        .secondsss     = RGB(127, 127,   0),
        .dowss         = RGB(127,   0, 101),
        .datess        = RGB( 54,  59, 127),
        .buffer        = RGB(157,   0,  79),
        .ip            = RGB(  0, 255,   0),
        .vol           = RGB(255,   0,   0),
        .rssi          = RGB(255, 255,   0),
        .battery       = RGB(255, 255,   0),
        .bitrate       = RGB(  0, 255, 255),
        .volbarout     = RGB(255,   0,   0),
        .volbarin      = RGB(255, 255,   0),
        .plcurrent     = RGB(  0,   0,   0),
        .plcurrentbg   = RGB( 91, 118, 255),
        .plcurrentfill = RGB( 91, 118, 255),
        .playlist      = {RGB(255, 255, 255), RGB(205, 205, 205), RGB(135, 135, 135), RGB( 85,  85,  85), RGB( 45,  45,  45)},
    },
    {   // vip-cxema.org
        .background    = RGB(  0,   0,   0),
        .meta          = RGB(251, 219,   0),
        .metabg        = RGB( 51,  51,  51),
        .metafill      = RGB( 51,  51,  51),
        .title1        = RGB(255, 255, 255),
        .title2        = RGB(185, 185, 185),
        .digit         = RGB(255, 255, 255),
        .div           = RGB(  0, 255,   0),
        .line          = RGB(  0, 255,   0),
        .weather       = RGB(  0, 200, 255),
        .vuaxis        = RGB(  0,  63,   0),
        .vupeak        = RGB(255, 255, 255),
        .vumax         = RGB(210,   0,   0),
        .vumin         = RGB(  0, 128,   0),
        .clock         = RGB( 60, 224,  33),
        .clockbg       = RGB( 0,  0,   0),
        .seconds       = RGB(  0, 255, 255),
        .dow           = RGB(240, 240, 240),
        .date          = RGB(255, 255, 255),
        .clockss       = RGB( 30, 112,  16),
        .clockbgss     = RGB(  4,  16,   2),
        .secondsss     = RGB(  0, 127, 127),
        .dowss         = RGB(120, 120, 120),
        .datess        = RGB(127, 127, 127),
        .buffer        = RGB(255, 105, 180),
        .ip            = RGB(165, 165, 165),
        .vol           = RGB(165, 165, 165),
        .rssi          = RGB(168, 165, 165),
        .battery       = RGB(168, 165, 165),
        .bitrate       = RGB(255, 215,   0),
        .volbarout     = RGB(198,  93,   0),
        .volbarin      = RGB(189, 189, 189),
        .plcurrent     = RGB(255, 219,   0),
        .plcurrentbg   = RGB(  0,   0,   0),
        .plcurrentfill = RGB(  0,   0,   0),
        .playlist      = {RGB(165, 165, 165), RGB(145, 145, 145), RGB(120, 120, 120), RGB(100, 100, 100), RGB( 80,  80,  80)},
    },
};

#endif
