#ifndef dspcolors_h
#define dspcolors_h
#pragma once

// Centralized display colour macros selected by active DSP model.
//
// Only four macros are consumed outside this file:
//   BOOT_PRG_COLOR / BOOT_TXT_COLOR - boot screen (core/display.cpp)
//   TFT_BG / TFT_FG                 - OLED theme init (tools/oledcolorfix.h,
//                                     displayN5110.cpp) and DspCore::clearDsp()
//
// Panel-specific palettes live with their own driver instead, so this file
// stays a minimal shared contract.  The grayscale OLED palette for SSD1327 is
// in displaySSD1327.cpp; SSD1322's lives in its driver plus the SSD1322 library.
#if DSP_MODEL==DSP_SH1106 || DSP_MODEL==DSP_SH1107
  #define BOOT_PRG_COLOR    SH110X_WHITE
  #define BOOT_TXT_COLOR    SH110X_WHITE
  #define TFT_BG            SH110X_BLACK
  #define TFT_FG            SH110X_WHITE

#elif DSP_MODEL==DSP_SSD1306 || DSP_MODEL==DSP_SSD1305 || DSP_MODEL==DSP_SSD1322 || DSP_MODEL==DSP_ST7920
  #define BOOT_PRG_COLOR    WHITE
  #define BOOT_TXT_COLOR    WHITE
  #define TFT_BG            BLACK
  #define TFT_FG            WHITE

#elif DSP_MODEL==DSP_NOKIA5110
  #define BOOT_PRG_COLOR    BLACK
  #define BOOT_TXT_COLOR    BLACK
  #define TFT_BG            WHITE
  #define TFT_FG            BLACK

#elif DSP_MODEL==DSP_SSD1327
  #define BOOT_PRG_COLOR    0x07
  #define BOOT_TXT_COLOR    0x3f
  #define TFT_BG            0x00
  #define TFT_FG            0x08

#elif DSP_MODEL==DSP_1602 || DSP_MODEL==DSP_2004
  #define BOOT_PRG_COLOR    0x1
  #define BOOT_TXT_COLOR    0x1

#else
  #define BOOT_PRG_COLOR    0xE68B
  #define BOOT_TXT_COLOR    0xFFFF
#endif

#ifndef TFT_BG
  #define TFT_BG            BLACK
#endif

#ifndef TFT_FG
  #define TFT_FG            WHITE
#endif

#endif
