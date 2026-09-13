# Layout Key — `src/displays/conf/display*conf.h`

Reference for the `display*conf.h` files: what each macro does, what every field in the
configuration structs means, and the conventions and traps that are not visible from the
inline comments. Read this before writing or editing a layout.

Everything here is verified against the current tree; where a value is empirical (rather
than derived) it is called out.

Related documents:

- [`.github/code-summary.md`](../../../.github/code-summary.md) — how the display layer works overall
- [`.github/code-issues.md`](../../../.github/code-issues.md) — known open issues
- [`plans/oled-vu-meter.md`](../../../plans/oled-vu-meter.md) — OLED VU work

---

## 1. How a conf file is chosen

**By resolution, not by display model.** [`dspconf.h`](../dspconf.h) selects the file from
`DSP_WIDTH`/`DSP_HEIGHT`, after first deciding the category:

| Category | Test | `SCROLLDELAY` | `SCROLLTIME` |
|---|---|---|---|
| Character LCD | `DSP_LCD` | 2000 | 300 (20x4) / 400 (16x2) |
| Mono OLED | `DSP_OLED` | 5000 | 180 (Nokia) / 250 (ST7920) / **20 (others)** |
| TFT | default | 5000 | 20 |

Then the resolution picks the file. For `DSP_OLED` at 128x64 that is
[`displayOLED128x64conf.h`](displayOLED128x64conf.h) — which is shared by **SH1106,
SH1107, SSD1305, SSD1306 and SSD1327**, both 1-bit and 4-bit grayscale panels. Any edit
there affects all of them.

Consequence: layout work is a **build-and-flash** loop, not a runtime setting. Only the
*layout index* and *theme index* are user-switchable at runtime.

---

## 2. The five parts of a conf file

1. **Geometry macros** — `TFT_FRAMEWDT`, `MAX_WIDTH`, `BOOTLOGOTOP`, `FONTSHIFT`, and the
   `*_SHARED` flags.
2. **`_bootConfig`** (`BootData`) — the AP/boot screen layout.
3. **`_layoutNames[]`** — display names for the layout list in the WebUI.
4. **`_layouts[]`** (`LayoutData`) — one entry per layout; the actual work.
5. **`*Fmt` strings** — `printf` formats used by the text widgets.

All of it is `const ... PROGMEM`, so it costs flash, not RAM.

---

## 3. Geometry macros

| Macro | Meaning |
|---|---|
| `DSP_WIDTH`, `DSP_HEIGHT` | Panel resolution. Set per model in [`options.h`](../../core/options.h) (OLED 128x64 by default); overridable in `myoptions.h`. Conf selection depends on these. |
| `TFT_FRAMEWDT` | **A border margin, not a width.** Despite the name it is the left/right inset in pixels, used as the default `left` for full-width widgets. `1` on this OLED = keep text off the very first/last pixel column. It is used on OLED and LCD confs too, so the `TFT_` prefix is historical and misleading. |
| `MAX_WIDTH` | `DSP_WIDTH - TFT_FRAMEWDT*2` — usable width inside the margin. Defined per conf because the margin differs (1 on this OLED, 10 on the 480x320 TFT). |
| `BOOTLOGOTOP` | `y` position for the boot logo. The logo image itself is chosen by resolution in [`dspfont.h`](../dspfont.h); for 128x64 it is `bootlogo/110x32mono.h`. |
| `FONTSHIFT` | Vertical offset compensating for the **clock font**'s metrics. `0` when `CLOCKFONT == YO_MONO`, otherwise `15`. Applied to `numConf.top`, `clockConf.top` and `vuConf.top` only. See §7 for why it matters. |
| `IP_WEATHER_SHARED` | `true` = the IP text and the weather share one bottom row: the IP is blanked while weather is showing, weather is paused in the volume page, and repaints are forced on return. Defaults to `false` in [`display.cpp`](../../core/display.cpp). |
| `RSSI_BATT_SHARED` | `true` = RSSI and battery share one row and are drawn alternately rather than over each other. Defaults to `false`. |

### `FONTSHIFT` in detail

[`options.h`](../../core/options.h) sets `CLOCKFONT` to `CHUNKY6` on the 128x64-class OLEDs
and `CHUNKY6_PX` elsewhere, unless `myoptions.h` overrides it. [`dspfont.h`](../dspfont.h)
then picks `TIME_SIZE` (15 for a 128x64 OLED), and **redefines it to 2** if
`CLOCKFONT == YO_MONO` on such a panel — meaning `YO_MONO` falls back to the ordinary
display font at textsize 2 rather than using the 15 px clock font.

So in practice on this file:

| `CLOCKFONT` | `FONTSHIFT` | `numConf.top` | `clockConf.top` / `vuConf.top` |
|---|---|---|---|
| `YO_MONO` | 0 | 28 | 38 |
| `CHUNKY6` / `CHUNKY6_PX` | 15 | 43 | 53 |

The `+FONTSHIFT` terms exist so widgets near the clock move down when the tall clock font is
in use. With the default `CHUNKY6` the clock and VU tops land at **53** on a 64 px panel, so
they sit in the last 11 rows — check anything you place there against the hardware. Whether
a given `FONTSHIFT` is exactly right is empirical: the clock font's baseline offset decides
it, and only a flash proves it.

---

## 4. Text metrics — what `fontsize` means

`fontsize` is a **multiplier of a 6x8 character cell**, not a point size. From
[`widgets.h`](../widgets/widgets.h) and `Widget::_charSize()`:

```
cell width  = fontsize * CHARWIDTH   // CHARWIDTH  = 6
cell height = fontsize * CHARHEIGHT  // CHARHEIGHT = 8
```

So `fontsize: 1` = a 6x8 cell, `fontsize: 2` = 12x16. This is used for `left`/`top`
arithmetic all over the confs (`FONTSHIFT`-style offsets, `TFT_FRAMEWDT*2`, etc.) and is
why odd numbers show up in positions.

Two derived heights worth knowing:

- Playlist row height: `textsize*(CHARHEIGHT-1) + textsize*4` = **11 x textsize**. With
  `playlistConf` textsize 1 that is 11 px.
- `NumWidget` uses `TIME_SIZE`, not its `fontsize`, for its text height.

---

## 5. Alignment

`WidgetAlign` is defined in [`widgetsconfig.h`](../widgets/widgetsconfig.h) — **not** by
Adafruit, despite the familiar names:

| Name | Value | Effect |
|---|---|---|
| `WA_LEFT` | 0 | `left` is the exact x of the text |
| `WA_CENTER` | 1 | centred within the widget's `width` |
| `WA_RIGHT` | 2 | right edge at `width - left` |

**Trap:** some widgets overload `align`. `VuWidget` treats *any non-zero* `align` as
"horizontal VU" and `WA_LEFT` as "vertical VU" — so on the VU, `align` is an orientation
selector, not just text alignment. When `rotateVU = true` this is bypassed and `align`
becomes irrelevant for the VU.

---

## 6. Configuration structs

Field order matters: these are aggregate-initialised positionally, and the `{ }` in the
confs map 1:1 to the declaration order in
[`widgetsconfig.h`](../displays/widgets/widgetsconfig.h).

### `WidgetConfig` — `{ left, top, fontsize, align }`

Plain text/positioned widget. Used directly by `bitrateConf`, `voltxtConf`,
`batteryConf`, `iptxtConf`, `rssiConf`, `numConf`, `clockConf`, `vuConf`.

### `ScrollConfig` — `{ widget, buffsize, uppercase, width, startscrolldelay, scrolldelta, scrolltime }`

| Field | Meaning |
|---|---|
| `widget` | the nested `WidgetConfig` |
| `buffsize` | text buffer size in bytes. Also the widget's "is configured" test (`> 0`). Must be large enough for long titles; CJK needs 3 bytes/char. |
| `uppercase` | **Currently has no effect.** The value is stored and `TextWidget::uppercase()` exposes it, but nothing reads it. Use `PRETEXT_ALLCAPS` in `myoptions.h` instead. |
| `width` | scrolling window width in pixels, used to decide whether scrolling is needed and to size the window buffer. Clamped to `MAX_WIDTH` by the widget. |
| `startscrolldelay` | ms held at the start position before scrolling begins. Confs pass `SCROLLDELAY` (or `SCROLLDELAY/5` for the playlist). |
| `scrolldelta` | pixels moved per step |
| `scrolltime` | ms per step after the initial hold |

Scroll speed in px/s = `scrolldelta * 1000 / scrolltime`. This file: title scrolls at
`1*1000/20 = 50 px/s`, the playlist at `3*1000/20 = 150 px/s`.

### `FillConfig` — `{ widget, width, height, outlined }`

Solid rectangle. `height > 0` is the "is configured" test. `outlined` draws a 1 px frame.
Used by `metaBGConf`, `metaBGConfInv`, `volbarConf`, `playlBGConf`, `bufferbarConf`.

`metaBGConfInv` is the band drawn behind the title when *invert title* is enabled; it is
only used if its own `height > 0`, otherwise `metaBGConf` is used instead.

### `BitrateConfig` — `{ widget, dimension }`

The "codec badge". `dimension > 0` is the test. If set, a `BitrateWidget` (codec name
badge) replaces the plain `bitrateConf` text.

### `VUBandsConfig` — `{ width, height, space, vspace, perheight, fadespeed }`

| Field | Meaning |
|---|---|
| `width` | thickness of **one** channel bar (L and R are drawn side by side, so total thickness = `width*2 + space`) |
| `height` | length of the bar |
| `space` | gap between the L and R bars |
| `vspace` | gap between the segments that make up a bar |
| `perheight` | segments per bar; segment step = `height / perheight` |
| `fadespeed` | pixels advanced **per display tick** when the bar is fading. Not per second — see below. |

Orientation: with `rotateVU = false` and `align = WA_LEFT` the bar runs vertically
(`height` = length, `width*2+space` = footprint width). With `rotateVU = true`, or with a
non-zero `align`, the bar runs horizontally (`height` = length along x, `width*2+space` =
footprint height). **Compute the footprint before choosing numbers** — it is the most
common way to push a VU off the screen.

`fadespeed` has no time base, so the same number looks different on every panel: a fast
refreshing OLED (~100 ticks/s) fades roughly ten times quicker than a slow TFT. Tune it per
conf; do not copy values between confs.

### `MoveConfig` — `{ x, y, width }`

Movement rectangle for the screensaver. Every shipped conf uses `{ 0, 0, -1 }`, and
`width = -1` is the documented "keep the conf position" (no movement).

### `ProgressConfig` — `{ speed, width, barwidth }`

Boot/update progress bar: `speed` = ms per step, `width` = total bar columns,
`barwidth` = thickness in px.

---

## 7. The two top-level tables

### `_bootConfig` (`BootData`)

The AP-mode / boot screen. Positions here are independent of `_layouts[]`, and its
`apSettConf` / `apPassConf` values are also reused by the update dialog on the volume page,
so changing them moves the "Updating files" text too.

- `apTitleConf`, `apSettConf` — `ScrollConfig` (title line, settings line)
- `bootstrConf`, `apNameConf`, `apName2Conf`, `apPassConf`, `apPass2Conf`, `bootWdtConf` — `WidgetConfig`
- `bootPrgConf` — `ProgressConfig`

### `_layoutNames[]` and `_layouts[]`

`_layoutNames` is the list shown in the WebUI layout selector; `_layouts` holds one
`LayoutData` per entry. Keep the two lists the same length and the same order — the layout
id is the array index.

`LayoutData` members, in declaration order (this is the order they must appear in the
designated initialiser):

| Member | Type | Purpose |
|---|---|---|
| `metaConf` | ScrollConfig | station/status line |
| `title1Conf` | ScrollConfig | title line 1 |
| `title2Conf` | ScrollConfig | title line 2 (optional) |
| `playlistConf` | ScrollConfig | playlist text |
| `weatherConf` | ScrollConfig | weather (optional) |
| `metaBGConf` | FillConfig | title band |
| `metaBGConfInv` | FillConfig | title band when *invert title* is on |
| `volbarConf` | FillConfig | volume slider (optional) |
| `playlBGConf` | FillConfig | playlist highlight fill |
| `bufferbarConf` | FillConfig | buffer bar (optional) |
| `bitrateConf` | WidgetConfig | bitrate text (or codec badge, see `fullbitrateConf`) |
| `voltxtConf` | WidgetConfig | volume number (optional) |
| `batteryConf` | WidgetConfig | battery (optional) |
| `iptxtConf` | WidgetConfig | IP address (optional) |
| `rssiConf` | WidgetConfig | WiFi signal (optional) |
| `numConf` | WidgetConfig | large volume/station number |
| `clockConf` | WidgetConfig | clock |
| `vuConf` | WidgetConfig | VU meter position |
| `fullbitrateConf` | BitrateConfig | codec badge; empty falls back to `bitrateConf` |
| `bandsConf` | VUBandsConfig | VU band geometry |
| `clockMove` | MoveConfig | clock movement (no VU position) |
| `weatherMove` | MoveConfig | weather movement (no VU position) |
| `weatherMoveVU` | MoveConfig | weather movement while the VU is shown |
| `boomboxStyle` | bool | VU drawn as a "boombox" horizontal meter |
| `rotateVU` | bool | VU rotated 90 degrees |

Booleans default to `false` when omitted, which is why most confs do not mention them.

---

## 8. The empty-config convention and the guard rule

`{ }` means "unused", e.g. `.fullbitrateConf = { }, // unused`. It is identical to omitting
the line: both leave every field zero, because these structs have no default member
initialisers. Commenting a line out is safe **as long as the remaining designated
initialisers stay in declaration order** — C++ requires that.

Each optional widget is created only when its own "is configured" field is set. Test the
meaningful field, never a coordinate (`{0,0}` is a legal position):

| Config type | Guard |
|---|---|
| `WidgetConfig` | `textsize > 0` |
| `ScrollConfig` | `buffsize > 0` |
| `FillConfig` | `height > 0` |
| `BitrateConfig` | `dimension > 0` |

An empty `vuConf` therefore means "no VU", and an empty `bandsConf` means nothing only
because the VU is not created in the first place.

---

## 9. Annotated walkthrough: `displayOLED128x64conf.h`

Screen: 128x64, margin `TFT_FRAMEWDT` = 1, so `MAX_WIDTH` = 126.

| `y` | Occupant | Source |
|---|---|---|
| 0-17 | title band | `metaBGConfInv` (`height` 17) |
| 1- | title text | `metaConf` top 1, fontsize 2 (12x16 cell) |
| 19 | title1 + bitrate | `title1Conf`, `bitrateConf` |
| 26-38 | playlist highlight | `playlBGConf` (overridden to the live row height) |
| 28 | title2 | `title2Conf` |
| 28+FONTSHIFT | number | `numConf` |
| 30-38 | playlist text | `playlistConf`, 11 px rows |
| 38+FONTSHIFT | clock (right) and VU (left) | `clockConf`, `vuConf` |
| 55 | battery / IP / RSSI / weather | shared bottom row |
| 63 | volume bar | `volbarConf`, full width, 1 px |

The VU as configured:

```c
.vuConf     = { TFT_FRAMEWDT, 38+FONTSHIFT, 1, WA_LEFT },
.bandsConf  = { 7, 44, 1, 1, 11, 3 },
.rotateVU   = true,
```

`rotateVU = true` makes it horizontal, so the footprint is `width*2+space = 15` px tall and
`height = 44` px long, placed at `x = 1`, `y = 38 + FONTSHIFT`. With the default
`CLOCKFONT` that is `y = 53`, ending at 68 — i.e. it overlaps the bottom row. With
`FONTSHIFT = 0` it would be `38..53` and clear. Verify against the hardware for whichever
`CLOCKFONT` is in use, and remember the VU shares its row with the right-aligned clock.

---

## 10. Checklist and gotchas

- **Changing a conf needs a rebuild and flash.** Nothing here is runtime-configurable.
- **A conf is shared across models of the same resolution.** Editing the 128x64 OLED file
  affects SH1106/SH1107/SSD1305/SSD1306/SSD1327.
- **Compute the VU footprint** (`width*2 + space` in the non-length axis) before picking
  numbers, and check it against `DSP_HEIGHT` and the row it shares.
- **`fontsize` is a 6x8 cell multiplier**, not points.
- **`TFT_FRAMEWDT` is a margin**, despite the name.
- **`uppercase` does nothing** — use `PRETEXT_ALLCAPS`.
- **`fadespeed` is per display tick**, not per second, so it is not portable between panels.
- **Designated initialisers must stay in declaration order.** Commenting a line out is
  fine; reordering is not.
- **`{ }` and an omitted line are equivalent.**
- **`_layoutNames` and `_layouts` must stay in step** — the index is the layout id.
- `playlBGConf.height` is overridden at runtime by the live playlist row height, so the
  value in the conf is only a fallback.
