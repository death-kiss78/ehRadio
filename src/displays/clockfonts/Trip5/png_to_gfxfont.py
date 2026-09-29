#!/usr/bin/env python3
"""
png_to_gfxfont.py — Convert a clock-digit PNG to an Adafruit GFX font header.

PNG layout (left to right):  0  1  2  3  4  5  6  7  8  9  :
  - White (or light) glyphs on a black background.
  - Each digit cell is 2× the width of the colon cell.
  - Auto-detect: image width must be divisible by 21
      (10 digits × 2 units + 1 colon × 1 unit = 21 units total)
  - Or pass --digit-width to specify the digit cell width in pixels.

Output:
  A .h file with Clock_GFXfont — matching the ehRadio DS_DIGI font convention.

Usage:
  python png_to_gfxfont.py input.png output.h

  python png_to_gfxfont.py input.png output.h --glow-char '/'

  python png_to_gfxfont.py input.png output.h --digit-width 40
"""

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("Pillow is required:  pip install Pillow", file=sys.stderr)
    sys.exit(1)

CHARS = list("0123456789:")
THRESHOLD = 32  # luminance above this = foreground (white on black)


# ---------------------------------------------------------------------------
# Glyph helpers
# ---------------------------------------------------------------------------

def bounding_box(pixels, x_start, x_end, height, threshold):
    """Return (top, bottom, left_rel, right_rel) of foreground pixels in cell."""
    top, bottom, left_rel, right_rel = height, -1, x_end - x_start, -1
    for y in range(height):
        for x in range(x_start, x_end):
            if pixels[x, y] > threshold:
                if y < top:        top       = y
                if y > bottom:     bottom    = y
                cx = x - x_start
                if cx < left_rel:  left_rel  = cx
                if cx > right_rel: right_rel = cx
    return top, bottom, left_rel, right_rel


def pack_glyph(pixels, x_start, y_top, y_bottom, x_left_rel, x_right_rel, threshold):
    """Pack glyph pixels MSB-first, continuous bit stream (no per-row padding)."""
    data = []
    byte, count = 0, 0
    for y in range(y_top, y_bottom + 1):
        for x in range(x_start + x_left_rel, x_start + x_right_rel + 1):
            byte = (byte << 1) | (1 if pixels[x, y] > threshold else 0)
            count += 1
            if count == 8:
                data.append(byte)
                byte, count = 0, 0
    if count:
        data.append(byte << (8 - count))  # final partial byte, zero-padded
    return data


def build_digit_union_glow(pixels, digit_width, height, threshold, tight_bounds):
    """Build a glow glyph as union of lit pixels across digits 0-9 (excluding colon)."""
    mask = [[0] * digit_width for _ in range(height)]

    # OR all 10 digit cells into a single digit-width mask.
    for d in range(10):
        x_start = d * digit_width
        for y in range(height):
            row = mask[y]
            for xr in range(digit_width):
                if pixels[x_start + xr, y] > threshold:
                    row[xr] = 1

    top, bottom, left, right = height, -1, digit_width, -1
    for y in range(height):
        for x in range(digit_width):
            if mask[y][x]:
                if y < top:
                    top = y
                if y > bottom:
                    bottom = y
                if x < left:
                    left = x
                if x > right:
                    right = x

    if bottom < 0:
        return dict(data=[], width=0, height=0, xOffset=0, yOffset=0)

    if not tight_bounds:
        left = 0
        right = digit_width - 1
        top = 0
        bottom = height - 1

    data = []
    byte, count = 0, 0
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            byte = (byte << 1) | (1 if mask[y][x] else 0)
            count += 1
            if count == 8:
                data.append(byte)
                byte, count = 0, 0
    if count:
        data.append(byte << (8 - count))

    return dict(
        data=data,
        width=(right - left + 1),
        height=(bottom - top + 1),
        xOffset=left,
        yOffset=-(bottom - top + 1),
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Convert clock-digit PNG (0-9 + colon) to Adafruit GFX font header.")
    parser.add_argument("input",  help="Input PNG file")
    parser.add_argument("output", help="Output .h file")
    parser.add_argument("--digit-width", type=int, default=None,
                        help="Digit cell width in pixels (colon = half this). "
                             "Auto-detected when image width is divisible by 21.")
    parser.add_argument("--glow-char", type=str, default=None,
                        help="Optional one-character ASCII slot (0x20..0x3A) to fill with "
                             "a synthetic full-on glyph, e.g. '/', '#', '\\\\'.")
    parser.add_argument("--threshold", type=int, default=THRESHOLD,
                        help="Foreground threshold (0..255). Lower values preserve anti-aliased edges.")
    parser.add_argument("--tight-bounds", action="store_true",
                        help="Use trimmed per-glyph bounds. Default keeps fixed cell metrics.")
    args = parser.parse_args()

    img = Image.open(args.input).convert("L")
    W, H = img.size
    pixels = img.load()

    # --- Determine cell widths -------------------------------------------------
    if args.digit_width:
        dw = args.digit_width
    elif W % 21 == 0:
        dw = 2 * (W // 21)
    else:
        dw = round(2 * W / 21)
        print(f"Warning: image width {W} is not divisible by 21; "
              f"guessing digit-width={dw}. Pass --digit-width to override.", file=sys.stderr)

    cw = dw // 2  # colon cell width
    expected_w = 10 * dw + cw
    if W != expected_w:
        print(f"Warning: image width {W} != expected {expected_w} "
              f"(10 × {dw} + {cw}). Check --digit-width.", file=sys.stderr)

    cell_widths = [dw] * 10 + [cw]

    # --- Extract glyphs -------------------------------------------------------
    glyphs = []
    x = 0
    for ch, cell_w in zip(CHARS, cell_widths):
        top, bot, lrel, rrel = bounding_box(pixels, x, x + cell_w, H, args.threshold)
        if bot < 0:
            print(f"Warning: no foreground pixels found for '{ch}'.", file=sys.stderr)
            glyphs.append(dict(char=ch, data=[], width=0, height=0,
                               xAdvance=cell_w, xOffset=0, yOffset=0))
        else:
            if args.tight_bounds:
                gw = rrel - lrel + 1
                gh = bot - top + 1
                data = pack_glyph(pixels, x, top, bot, lrel, rrel, args.threshold)
                yo = -gh
                xo = lrel
            else:
                gw = cell_w
                gh = H
                data = pack_glyph(pixels, x, 0, H - 1, 0, cell_w - 1, args.threshold)
                yo = -H
                xo = 0
            glyphs.append(dict(char=ch, data=data, width=gw, height=gh,
                               xAdvance=cell_w, xOffset=xo, yOffset=yo))
        x += cell_w

    # --- Build flat bitmap + per-glyph offsets --------------------------------
    flat_bmp = []
    offsets  = []
    for g in glyphs:
        offsets.append(len(flat_bmp))
        flat_bmp.extend(g["data"])

    # Optional synthetic glow glyph injected at a selected ASCII slot.
    glow_code = None
    glow_entry = None
    if args.glow_char is not None:
        if len(args.glow_char) != 1:
            print("Error: --glow-char must be exactly one character.", file=sys.stderr)
            sys.exit(2)
        glow_code = ord(args.glow_char)
        if glow_code < 0x20 or glow_code > 0x3A:
            print("Error: --glow-char must be in ASCII range 0x20..0x3A.", file=sys.stderr)
            sys.exit(2)
        glow_glyph = build_digit_union_glow(pixels, dw, H, args.threshold, args.tight_bounds)
        if glow_glyph["width"] == 0 or glow_glyph["height"] == 0:
            print("Warning: glow mask is empty; using empty glyph.", file=sys.stderr)
        glow_entry = {
            "char": args.glow_char,
            "offset": len(flat_bmp),
            "width": glow_glyph["width"],
            "height": glow_glyph["height"],
            "xAdvance": dw,
            "xOffset": glow_glyph["xOffset"],
            "yOffset": glow_glyph["yOffset"],
        }
        flat_bmp.extend(glow_glyph["data"])

    # --- Derive C identifier prefix from output filename ----------------------
    stem   = os.path.splitext(os.path.basename(args.output))[0]
    prefix = "".join(c if c.isalnum() or c == "_" else "_" for c in stem)

    y_advance = H + max(1, H // 8)  # line height with a small gap

    # --- Build output ---------------------------------------------------------
    out = []

    # Bitmap array
    out.append(f"const uint8_t {prefix}Bitmaps[] PROGMEM = {{")
    for i in range(0, len(flat_bmp), 12):
        chunk = flat_bmp[i:i+12]
        out.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    # Replace trailing comma on last data line with nothing
    if out[-1].endswith(","):
        out[-1] = out[-1][:-1]
    out.append("};")
    out.append("")

    # Glyph table: 0x20 (space) … 0x3A (':')
    out.append(f"const GFXglyph {prefix}Glyphs[] PROGMEM = {{")
    glyph_by_code = {
        0x30 + i: {
            "offset": offsets[i],
            "width": glyphs[i]["width"],
            "height": glyphs[i]["height"],
            "xAdvance": glyphs[i]["xAdvance"],
            "xOffset": glyphs[i]["xOffset"],
            "yOffset": glyphs[i]["yOffset"],
        }
        for i in range(10)
    }
    glyph_by_code[0x3A] = {
        "offset": offsets[10],
        "width": glyphs[10]["width"],
        "height": glyphs[10]["height"],
        "xAdvance": glyphs[10]["xAdvance"],
        "xOffset": glyphs[10]["xOffset"],
        "yOffset": glyphs[10]["yOffset"],
    }
    if glow_entry is not None:
        glyph_by_code[glow_code] = glow_entry

    for code in range(0x20, 0x3B):
        is_last = code == 0x3A
        suffix = "" if is_last else ","
        if code in glyph_by_code:
            g = glyph_by_code[code]
            out.append(
                f"  {{ {g['offset']:6d}, {g['width']:3d}, {g['height']:3d}, {g['xAdvance']:3d}, "
                f"{g['xOffset']:4d}, {g['yOffset']:4d} }}{suffix}   // 0x{code:02X} '{chr(code)}'"
            )
        elif code == 0x20:
            # Space gets non-zero xAdvance so text layout remains predictable.
            out.append(f"  {{     0,   0,   0, {cw:3d},    0,    1 }}{suffix}   // 0x20 ' '")
        else:
            out.append(f"  {{     0,   0,   0,   0,    0,    0 }}{suffix}   // 0x{code:02X} '{chr(code)}'")
    out.append("};")
    out.append("")

    # GFXfont struct
    out.append(f"const GFXfont Clock_GFXfont PROGMEM = {{")
    out.append(f"  (uint8_t  *){prefix}Bitmaps,")
    out.append(f"  (GFXglyph *){prefix}Glyphs, 0x20, 0x3A, {y_advance} }};")
    out.append("")

    with open(args.output, "w") as f:
        f.write("\n".join(out))

    # --- Summary --------------------------------------------------------------
    print(f"Written: {args.output}")
    print(f"  Image: {W}×{H} px   digit-width: {dw}   colon-width: {cw}")
    print(f"  Threshold: {args.threshold}")
    print(f"  Bounds mode: {'tight' if args.tight_bounds else 'fixed-cell'}")
    print(f"  Bitmap bytes: {len(flat_bmp)}   yAdvance: {y_advance}")
    print(f"  Glyphs detected:")
    for g in glyphs:
        print(f"    '{g['char']}' → {g['width']}×{g['height']}  "
              f"xAdv={g['xAdvance']}  xOff={g['xOffset']}  yOff={g['yOffset']}")
    if glow_entry is not None:
        print(f"  Glow slot: '{args.glow_char}' (0x{glow_code:02X}) → "
              f"{glow_entry['width']}x{glow_entry['height']} digit-union mask")


if __name__ == "__main__":
    main()
