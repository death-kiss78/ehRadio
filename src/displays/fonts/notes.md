# Unicode GFXfont Rendering in ehRadio — Implementation Notes

This document captures what we learned while replacing the Adafruit GFX Library's
built-in 256-slot `glcdfont` with a custom Unicode `GFXfont` that can render
400–1421 glyphs covering Latin, Cyrillic, Greek, and extended punctuation.

## Table of Contents

1. [Why Replace glcdfont?](#why-replace-glcdfont)
2. [Adafruit GFXfont Format Primer](#adafruit-gfxfont-format-primer)
3. [BDF → GFXfont Conversion](#bdf--gfxfont-conversion)
4. [Overriding `write()` and `_writeGlyph()`](#overriding-write-and-_writeglyph)
5. [Critical Rendering Issues and Fixes](#critical-rendering-issues-and-fixes)
6. [UTF-8 Decoder](#utf-8-decoder)
7. [Icon Rendering via Control Characters](#icon-rendering-via-control-characters)
8. [Clock Font Dispatch](#clock-font-dispatch)
9. [The `preText` Pipeline](#the-pretext-pipeline)
10. [PSRAM Framebuffer Considerations](#psram-framebuffer-considerations)
11. [PROGMEM and Flash Size](#progmem-and-flash-size)
12. [Integration Checklist](#integration-checklist)

---

## Why Replace glcdfont?

The stock Adafruit GFX Library's `glcdfont.c` is a hardcoded 256-slot font
(5×7 pixels). It covers only ASCII 0x00–0xFF and uses codepage tricks to
swap character sets at build time.

**Problems with glcdfont:**
- Requires compile-time codepage selection (Latin OR Cyrillic, never both)
- Cannot render accented Latin (é, ñ, ü), Greek, or mixed scripts
- Users see garbage or blanks for non-ASCII text
- No runtime font switching

**The Unicode GFXfont approach:**
- One font file with glyphs for codepoints 0x0020–0x04FF (Latin + Cyrillic + Greek)
- 400+ glyphs (MatrixLight/MatrixChunky) or 1421 glyphs (X11)
- Render any supported script at runtime — no codepage switching
- Accent folding fallback for missing glyphs
- Same code path for all display drivers (OLED, TFT, LCD)

---

## Adafruit GFXfont Format Primer

A `GFXfont` is a C struct declared in PROGMEM:

```c
typedef struct {
  uint8_t  *bitmap;   // packed 1bpp bitmap data for all glyphs (MSB first per byte)
  GFXglyph *glyph;    // array of glyph descriptors, one per codepoint
  uint16_t  first;    // first codepoint in the glyph array
  uint16_t  last;     // last codepoint in the glyph array
  uint8_t   yAdvance; // vertical line spacing (cursor_y step after newline)
} GFXfont;
```

Each `GFXglyph` descriptor:
```c
typedef struct {
  uint16_t bitmapOffset; // byte offset into font->bitmap
  uint8_t  width;        // bitmap width in pixels
  uint8_t  height;       // bitmap height in pixels
  uint8_t  xAdvance;     // cursor_x advance after rendering this glyph
  int8_t   xOffset;      // horizontal offset from cursor_x to first pixel column
  int8_t   yOffset;      // vertical offset from baseline (positive = below)
} GFXglyph;
```

**Critical detail**: `xOffset + width` can exceed `xAdvance`. This is valid in
the GFXfont spec (glyphs can have overhang for decorative elements), but causes
pixel bleed with background-fill rendering. See [Glyph Bleed Clip](#glyph-bleed-clip).

---

## BDF → GFXfont Conversion

We use [`bdf2adafruit3.py`](bdf2adafruit3.py) to convert `.bdf` bitmap font files
to Adafruit GFXfont `.h` headers.

### Font selection requirements

The converter targets a **6×8 pixel cell** matching the project's `CHARWIDTH`:

| Property | Value | Notes |
|----------|-------|-------|
| Target width | 6 pixels | Must be uniform (no variable-width fonts) |
| Target height | 8 pixels | Matches glcdfont line height |
| yAdvance | 8 | Cursor Y step after each glyph |
| Glyph encoding | Unicode | BDF `ENCODING` field |

Accepted font bounding-box sizes (automatic or interactive padding/cutting):
- Width: 5–9 pixels
- Height: 6–10 pixels

### Usage

```bash
# Convert a BDF file covering codepoints U+0020 through U+04FF
py bdf2adafruit3.py MatrixChunky8.bdf 0x0020 0x04FF -o MatrixChunky8x6.h

# Non-interactive mode (accepts defaults)
py bdf2adafruit3.py MatrixLight8.bdf 0x20 0x04FF --auto -o MatrixLight8x6.h
```

### How normalisation works

If the font's native glyph size differs from 6×8, the converter applies:
- **Height**: trim bottom rows or pad with empty rows
- **Width**: center within 6 columns (pad left/right equally)
- **xOffset adjusted** to keep glyphs visually centered after padding

This preserves the visual appearance while fitting the fixed 6×8 cell.

---

## Overriding `write()` and `_writeGlyph()`

The stock `Adafruit_GFX::write(uint8_t c)` only handles single-byte ASCII.
To render Unicode characters (multi-byte UTF-8), you must override `write()`
and provide a custom glyph renderer.

### Where the override lives

In [`commongfx.h`](../tools/commongfx.h), `DspCore` inherits from `yoDisplay`
(which inherits from `Adafruit_GFX`):

```cpp
// UTF-8 aware write() — replaces the stock library's uint8_t-limited version.
using Print::write;
size_t write(uint8_t c) {
    // ... UTF-8 decoder (see section below) ...
}
```

And the private glyph renderer:

```cpp
void _writeGlyph(uint16_t cp) {
    const GFXfont *f = &DisplayFont;
    // ... glyph lookup, rendering, fallback ...
}
```

### Key design decisions

1. **Background fill on every glyph**: Unlike `Adafruit_GFX::drawChar()` which
   only draws foreground pixels, our `_writeGlyph` draws background pixels too
   whenever `textbgcolor != textcolor`. This is essential for clear text on
   non-uniform backgrounds (widget bars, weather panels).

2. **Using `writePixel`/`writeFillRect` directly**: We bypass `drawPixel` to
   avoid recursion through the library's own `write()` path, which would
   re-enter our UTF-8 decoder.

3. **`startWrite()`/`endWrite()` wrapping**: Required by Adafruit SPI TFT
   drivers (begin/end SPI transaction). No-ops on I2C OLEDs. Must wrap every
   rendering block that touches the display.

---

## Critical Rendering Issues and Fixes

### Glyph Bleed Clip

**Problem**: Some GFXfont glyphs have `xOffset + width > xAdvance`.
When rendering with background fill, the extra pixel columns bleed into
the next character's cell, causing smearing (most visible on clock colon
rendering).

**Fix** (in `_writeGlyph`, both `commongfx.h` and `psframebuffer.h`):

```cpp
for (uint8_t yy = 0; yy < h; yy++) {
    for (uint8_t xx = 0; xx < w; xx++) {
        if (bit == 0) { bits = pgm_read_byte(&bitmap[bo++]); bit = 0x80; }
        // Only render columns within xAdvance boundary
        if ((int16_t)(xo + xx) < (int16_t)pgm_read_byte(&glyph->xAdvance)) {
            if (bits & bit) {
                // foreground pixel
                writePixel(cursor_x + xo + xx, renderY + yo + yy, textcolor);
            } else if (textbgcolor != textcolor) {
                // background pixel
                writePixel(cursor_x + xo + xx, renderY + yo + yy, textbgcolor);
            }
        }
        bit >>= 1;
    }
}
```

**Important**: The bitmap must still consume ALL bits (the `bits`/`bit`
machinery advances `bo` for every column). Only the rendering is clipped.
Skipping columns entirely would misalign the bit counter.

### Space Handling

The stock library handles space (0x20) as a special case. Some GFXfonts
start at codepoint 0x21 (exclamation mark), so space would fall through
to `foldAccent()` and never advance the cursor — causing characters to
render on top of each other.

**Fix**: Explicit space check before any font lookup:

```cpp
if (cp == ' ') {
    uint8_t spaceAdv = pgm_read_byte(&((GFXglyph *)pgm_read_ptr(&f->glyph))->xAdvance);
    cursor_x += (int16_t)spaceAdv * textsize_x;
    return;
}
```

### Unrenderable Codepoints

If a codepoint is not in the font AND no accent-folding fallback exists,
we still advance the cursor by one `xAdvance` — otherwise the cursor
stagnates and subsequent characters pile up at the same position.

```cpp
// Unrenderable codepoint — advance by one character cell
uint8_t spaceAdv = pgm_read_byte(&((GFXglyph *)pgm_read_ptr(&f->glyph))->xAdvance);
cursor_x += (int16_t)spaceAdv * textsize_x;
```

---

## UTF-8 Decoder

The `write(uint8_t c)` method decodes multi-byte UTF-8 sequences into a
full 16-bit codepoint, then calls `_writeGlyph(cp)`:

```cpp
size_t write(uint8_t c) {
    if (c < 0x80) {
        // Single byte (ASCII): decode and render immediately
        _utf8_cp = c;
        _utf8_remaining = 0;
        _writeGlyph(_utf8_cp);
    } else if (c < 0xC0) {
        // Continuation byte (10xxxxxx)
        if (_utf8_remaining > 0) {
            _utf8_cp = (_utf8_cp << 6) | (c & 0x3F);
            if (--_utf8_remaining == 0) _writeGlyph(_utf8_cp);
        }
    } else if (c < 0xE0) {
        // 2-byte sequence leader (110xxxxx)
        _utf8_cp = c & 0x1F;
        _utf8_remaining = 1;
    } else if (c < 0xF0) {
        // 3-byte sequence leader (1110xxxx)
        _utf8_cp = c & 0x0F;
        _utf8_remaining = 2;
    } else {
        // 4-byte sequence leader (11110xxx)
        _utf8_cp = c & 0x07;
        _utf8_remaining = 3;
    }
    return 1;
}
```

Supports up to 4-byte UTF-8 sequences (U+0000 through U+10FFFF).
Most practical use is 2-byte (Latin Supplement, Greek) and 3-byte (Cyrillic).

### `resetUTF8()`

Scroll widgets call `print()` in a loop to render each frame. If a UTF-8
sequence is interrupted mid-character at the end of one frame, the decoder
state carries over and corrupts the first glyph of the next frame.

```cpp
void resetUTF8() { _utf8_remaining = 0; }
```

Call this **before every `print()`** in scroll/repeat rendering loops.

---

## Icon Rendering via Control Characters

Codepoints 0x01–0x1F (ASCII control characters) are repurposed for inline
icon rendering. Each value indexes into `ICON_TABLE[]` from
[`icons.h`](../icons.h), which points to 6×8-pixel PROGMEM bitmaps.

```cpp
// Icon codepoints (0x01-0x1F) — render directly from ICON_TABLE
if (cp >= 0x01 && cp <= 0x1F) {
    const uint8_t* const *table = ICON_TABLE;
    if (cp < 32) {
        const uint8_t* icon = (const uint8_t*)pgm_read_ptr(&table[cp]);
        if (icon) {
            startWrite();
            // ... render 6x8 bitmap at cursor ...
            endWrite();
            cursor_x += 6 * textsize_x;
        }
    }
    return;
}
```

This allows embedding icons in display strings: `"\x01WiFi: %s"` renders
icon codepoint 0x01 (WiFi) followed by text. Icons use the same 6×8 cell
as the font for consistent layout.

### DSP_PIXEL_SPACER

Control character `0x1E` (Record Separator) is used as a 2-pixel spacer:

```cpp
#define DSP_PIXEL_SPACER '\x1E'
```

The clock widget inserts this between colon and digits for fine-tuned spacing.

---

## Clock Font Dispatch

The clock uses special large GFXfonts (not the display font). The dispatch
logic in `_writeGlyph` checks the `gfxFont` pointer:

```cpp
// If a clock font is active (not ours, not NULL), let the library handle it.
if (gfxFont != NULL && gfxFont != (GFXfont *)f) {
    Adafruit_GFX::write((uint8_t)(cp & 0xFF));
    return;
}
```

The clock font is set via `setFont(&clockFont)`, which stores the pointer in
`gfxFont`. Since `gfxFont != &DisplayFont`, the glyph is delegated to the
stock `Adafruit_GFX::write()` — which only handles single-byte characters.
This is fine because clock digits are ASCII 0x30–0x39.

The `&DisplayFont` cast to `(GFXfont *)f` ensures we detect when no special
font is active (NULL) or when the display font itself was set via `setFont()`
(both cases use our own `_writeGlyph` path).

---

## The `preText` Pipeline

Before rendering, every codepoint passes through `preText()` in
[`pretext.cpp`](../tools/pretext.cpp). This is a chain of optional
preprocessors controlled by compile-time macros:

```cpp
uint16_t preText(uint16_t cp, const GFXfont *font) {
    #ifdef PRETEXT_ALLCAPS
        cp = allCaps(cp);
    #endif
    #ifdef PRETEXT_FOLDACCENT
        cp = foldAccent(cp, font);
    #endif
    return cp;
}
```

### `allCaps()` — Uppercase Conversion

When `PRETEXT_ALLCAPS` is defined, converts lowercase to uppercase for
scripts where the font only has uppercase glyphs. Covers:
- ASCII a–z → A–Z
- Latin-1 Supplement (à–ÿ → À–Ÿ)
- Latin Extended-A (ā–ž → Ā–Ž)
- Cyrillic (а–я → А–Я, ё → Ё)

### `foldAccent()` — Diacritic Stripping

When `PRETEXT_FOLDACCENT` is defined, strips diacritical marks from
Latin-1 and Latin Extended-A characters if the accented glyph is NOT
present in the font. Maps é→e, ñ→n, ü→u, etc.

```cpp
// First check: does the glyph exist in the font?
if (cp >= font->first && cp <= font->last) {
    uint16_t idx = cp - font->first;
    if (pgm_read_byte(&font->glyph[idx].width) > 0 &&
        pgm_read_byte(&font->glyph[idx].height) > 0)
        return cp;  // glyph exists, keep as-is
}
// Glyph not in font — try accent folding
if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
// ... etc ...
```

**Important**: `foldAccent` proactively checks existence BEFORE folding.
If the accented glyph exists (even with w==0 or h==0 as a placeholder slot),
it returns unchanged. Only truly missing glyphs get folded.

This allows a font to include common accented characters (é, ñ) while
falling back to ASCII for rare ones (ŵ, ṽ).

---

## PSRAM Framebuffer Considerations

When using a PSRAM framebuffer (`PSFBUFFER` defined), display output goes
through a memory buffer that is flushed to the physical display in one
operation. The framebuffer class in [`psframebuffer.h`](../tools/psframebuffer.h)
needs its own **identical** `_writeGlyph()` implementation.

**Why**: The framebuffer `write()` cannot delegate back to `DspCore::write()`
because the pixel-drawing functions differ (`drawPixel` vs `writePixel`).
The glyph rendering logic must be duplicated — any fix applied to one must
be applied to both.

**Key differences**:
- PSRAM version uses `drawPixel()` (draws into PSRAM buffer)
- Direct version uses `writePixel()`/`writeFillRect()` (draws to display)
- PSRAM version doesn't call `startWrite()`/`endWrite()` (not needed)

---

## PROGMEM and Flash Size

All font data is stored in flash via PROGMEM:

- Bitmap data: `const uint8_t FontNameBitmaps[] PROGMEM`
- Glyph descriptors: `const GFXglyph FontNameGlyphs[] PROGMEM`
- Font struct: `const GFXfont FontName PROGMEM`

At runtime, access via `pgm_read_byte()`, `pgm_read_word()`, `pgm_read_ptr()`.

**Font sizes** (measured, not estimated):

| Font | Glyphs (non-empty / slots) | Bitmaps | Glyph table | Total |
|------|---------------------------|---------|-------------|-------|
| MatrixLight8x6 | 460 / 1247 (0x21-0x4FF) | 2,760 B | 9,976 B | 12,752 B |
| MatrixChunky8x6 | 460 / 1247 (0x21-0x4FF) | 2,760 B | 9,976 B | 12,752 B |
| UnixX11_6x9 (`Fixed`) | 618 / 1248 (0x20-0x4FF) | 3,696 B | 9,984 B | 13,696 B |

Totals include the 16-byte `GFXfont` struct. The glyph table costs
`slots * 8`, not `slots * 6`: `GFXglyph` is 7 bytes of payload
(uint16_t + 5 x uint8_t) with 2-byte alignment, so the linker emits one
padding byte per slot. The `// N bytes glyph table` line that
bdf2adafruit3.py writes at the end of each font uses `range_size * 6`
and therefore understates every 1247-slot font by 2,494 bytes.

Only one font is compiled in (`DISPLAYFONT` selects it in dspfont.h);
the other two cost nothing in the .bin. MatrixLight and MatrixChunky
differ only in their 2,760-byte bitmap arrays — their glyph tables are
identical, so a shared-table swap keeps one 9,976-byte table plus
2,760 bytes per additional font of the same 6x8 metric class.

The font is selected at compile time via `DISPLAYFONT` in `myoptions.h`,
resolved in [`dspfont.h`](../dspfont.h):

```cpp
#if DISPLAYFONT == MATRIXLIGHT
    #include "fonts/MatrixLight8x6.h"
    #define DisplayFont MatrixLight8x6
#elif DISPLAYFONT == MATRIXCHUNKY
    #include "fonts/MatrixChunky8x6.h"
    #define DisplayFont MatrixChunky8x6
#elif DISPLAYFONT == X11
    #include "fonts/UnixX11_6x9.h"
    #define DisplayFont Fixed
#endif
```

---

## Integration Checklist

When adding Unicode GFXfont support to a new display driver or project:

1. **Include the font header** via `dspfont.h` (or equivalent selection logic)
2. **Override `write(uint8_t)`** with a UTF-8 decoder (see [UTF-8 Decoder](#utf-8-decoder))
3. **Implement `_writeGlyph(uint16_t cp)`** with:
   - Newline/carriage-return handling
   - Icon codepoint rendering (if using icons)
   - Space special case
   - `preText()` call before glyph lookup
   - Clock/external font dispatch check
   - Glyph bleed clip: `if ((xo + xx) < xAdvance)` before rendering
   - Background fill: `else if (textbgcolor != textcolor)`
   - `foldAccent()` fallback for missing glyphs
   - Cursor advance for unrenderable codepoints
4. **Wrap rendering blocks** in `startWrite()`/`endWrite()` (SPI TFTs) — skip for OLEDs
5. **Call `resetUTF8()`** before every `print()` in scroll/repeat rendering
6. **Duplicate `_writeGlyph`** in the PSRAM framebuffer class if using one
7. **Use `pgm_read_*` macros** for all PROGMEM access — never dereference directly
8. **Handle `textsize_x`/`textsize_y`** scaling in pixel/rect drawing calls
9. **Use `writePixel`/`writeFillRect`** (not `drawPixel`) to avoid recursion
10. **Test with mixed-script text**: ASCII + Cyrillic + accented Latin + icons

---

## Reference Files

| File | Purpose |
|------|---------|
| [`bdf2adafruit3.py`](bdf2adafruit3.py) | BDF → GFXfont converter (target 6×8 cell) |
| [`commongfx.h`](../tools/commongfx.h) | `DspCore` with `write()` and `_writeGlyph()` |
| [`psframebuffer.h`](../tools/psframebuffer.h) | PSRAM framebuffer with duplicated `_writeGlyph()` |
| [`pretext.h`](../tools/pretext.h) / [`pretext.cpp`](../tools/pretext.cpp) | `preText()`, `allCaps()`, `foldAccent()`, `utf8_strlen()` |
| [`dspfont.h`](../dspfont.h) | Font selection macros (`DISPLAYFONT`, `DisplayFont`) |
| [`icons.h`](../icons.h) | Icon bitmaps and `ICON_TABLE[]` |
| [`MatrixLight8x6.h`](MatrixLight8x6.h) | GFXfont: MatrixLight 6×8, ~400 glyphs |
| [`MatrixChunky8x6.h`](MatrixChunky8x6.h) | GFXfont: MatrixChunky 6×8, ~400 glyphs |
| [`UnixX11_6x9.h`](UnixX11_6x9.h) | GFXfont: Unix X11 fixed 6×9, 1421 glyphs |
