# Layout Key — how to build a layout

A **layout** is one entry in the `_layouts[]` array of a `display*conf.h` file. It decides
where each widget sits, which ones exist at all, and how the VU meter is drawn.

This guide lists the fields, explains the numbers in plain language, and points out the quirks
that are not obvious from the conf file itself.

If you are converting a layout from another firmware instead of writing one, see
[`conf_tool.md`](conf_tool.md).

---

## 1. Which file do I edit?

The conf file is chosen by **resolution**, not by panel model. A 128x64 OLED picks
[`displayOLED128x64conf.h`](displayOLED128x64conf.h), a 480x320 TFT picks
[`displayTFT480x320conf.h`](displayTFT480x320conf.h), and so on.

**One file serves every panel of that size.** Editing the 128x64 OLED file changes SH1106,
SH1107, SSD1305, SSD1306 and SSD1327 displays alike, so check your layout on the screen you
actually have.

Only the **layout index** and the **theme** can be changed at runtime, from the WebUI. Anything
in the conf file needs a **rebuild and flash**.

---

## 2. Quick start

Every conf file has these parts. `_layoutNames[]` is the list the WebUI shows; `_layouts[]` holds
the actual layouts, one entry each:

```c
const char _layoutNames[][64] PROGMEM = {
    "Default",          // shown in the WebUI layout list
    "Big VU",
};

const LayoutData _layouts[] PROGMEM = {
    {   // Default
        /* SCROLLS   {{ left, top, fontsize, align }, buffsize, uppercase, width, scrolldelay, scrolldelta, scrolltime } */
        .metaConf      = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 2, WA_LEFT }, 140, true, MAX_WIDTH, SCROLLDELAY, 2, SCROLLTIME },
        .title1Conf    = {{ TFT_FRAMEWDT, 19, 1, WA_LEFT }, 140, true, MAX_WIDTH-24, SCROLLDELAY, 1, SCROLLTIME },
        .playlistConf  = {{ TFT_FRAMEWDT, 30, 1, WA_LEFT }, 140, true, MAX_WIDTH, SCROLLDELAY/5, 1, SCROLLTIME },
        /* WIDGETS   { left, top, fontsize, align } */
        .bitrateConf   = { 0, 19, 1, WA_RIGHT },
        .clockConf     = { TFT_FRAMEWDT, 38+FONTSHIFT, 0, WA_CENTER },
        /* SLIDER BARS {{ left, top, fontsize, align }, width, height, outlined } */
        .volbarConf    = {{ 0, 64-1, 0, WA_LEFT }, DSP_WIDTH, 1, false },
        .rotateVU      = true,
    },
};
```

Four rules that break the build, or the reading, if you ignore them:

- **`_layoutNames[]` and `_layouts[]` must stay the same length**, in the same order. The
  layout's index is its position in both arrays.
- **Write the fields in the order listed in §3.** These are designated initialisers, so a field
  that comes earlier in the struct must be written earlier in the entry. Commenting a line out
  is fine; swapping two lines is not.
- **Write every field.** A field left out is not an error - it is zero, exactly like `{ }` - but
  the shipped confs write them all, so that one file can be read as a whole instead of being
  compared against another. `conf_tool.py --clean` fills in whatever is missing.
- **Keep the section headers.** `/* SCROLLS ... */`, `/* SLIDER BARS ... */`,
  `/* LINES + RECTANGLES ... */`, `/* WIDGETS ... */`, `/* CODEC BADGE ... */`, `/* VU BANDS ... */`,
  `/* MOVES ... */` and `/* TRANSFORMS ... */` are
  part of the master too, and `conf_tool.py --clean` restores and normalises them.

---

## 3. The fields, in the order they appear

This list is `struct LayoutData` in [`widgetsconfig.h`](../widgets/widgetsconfig.h), which is the
master for the field order, the types and the section headers - the same order the compiler
demands of a designated initialiser. Add a field there and re-run `conf_tool.py --clean` to bring
the conf files back into line.

`WidgetConfig` — a simple positioned widget. All four numbers: `{ left, top, fontsize, align }`.

`ScrollConfig` — a line of text that can scroll. `{ { left, top, fontsize, align }, buffsize,
uppercase, width, scrolldelay, scrolldelta, scrolltime }`.

`FillConfig` — a solid rectangle. `{ { left, top, fontsize, align }, width, height, outlined }`.
Only `left`, `top`, `width` and `height` matter to a plain fill. **`outlined` is read by the two
sliders only** — `volbarConf` and `bufferbarConf`, where it is a one-pixel frame plus an inset for
the bar inside it. `metaBGConf`, `metaBGConfInv`, `playlBGConf`, `underLineConf` and `overLineConf`
are plain fills and ignore it, so a `true` there does nothing.

`VUBandsConfig` — the VU bar's geometry, five numbers (see §6).

`MoveConfig` — where a widget travels while the screensaver runs (see §7).

`BitrateConfig` — `{ { left, top, fontsize, align }, dimension }`, the codec badge.

| Field | Type | What it is |
|---|---|---|
| `metaConf` | Scroll | Station name / status line. **Required** — dialogs write into it. |
| `title1Conf` | Scroll | Title line 1 |
| `title2Conf` | Scroll | Title line 2 |
| `playlistConf` | Scroll | The playlist text. **Required** |
| `weatherConf` | Scroll | Weather line |
| `volbarConf` | Fill | Volume slider — one of the two sliders, where `outlined` draws a frame |
| `bufferbarConf` | Fill | Stream buffer bar — the other slider |
| `metaBGConf` | Fill | The band behind the title, or the rule under it, whichever the layout wants |
| `metaBGConfInv` | Fill | Used **instead of** `metaBGConf` when *invert title* is on (the pointer is re-pointed at it), and its colour becomes the divider's; if empty, `metaBGConf` is used |
| `underLineConf` | Rect | A rectangle drawn **under** the page — added early, so the text and the VU paint over it |
| `overLineConf` | Rect | A rectangle drawn **over** the page — added last, so it lands on top of them |
| `playlBGConf` | Fill | Highlight behind the current playlist row |

The last five are **plain filled rectangles**. `width` and `height` can be anything — a `1` makes a
line, a large pair makes a panel or a band — and the fourth value is unused: `outlined` belongs to the
two sliders above, where it draws a frame and insets the bar inside it. Writing `false` there is the
honest reading of `{{ ..., width, height, false }`.
| `bitrateConf` | Widget | Bitrate text (replaced by a codec badge if `fullbitrateConf` is set) |
| `voltxtConf` | Widget | Volume number |
| `batteryConf` | Widget | Battery |
| `iptxtConf` | Widget | IP address |
| `rssiConf` | Widget | WiFi signal |
| `numConf` | Widget | The large volume/station number |
| `clockConf` | Widget | The clock |
| `vuConf` | Widget | Where the VU meter sits |
| `fullbitrateConf` | Bitrate | Codec name badge |
| `bandsConf` | VUBands | The VU bar's shape |
| `clockMove` | Move | Clock travel while the screensaver runs |
| `weatherMove` | Move | Weather travel, when no VU is shown |
| `weatherMoveVU` | Move | Weather travel, while the VU is shown |
| `boomboxVU` | bool | Draw the VU as a "boombox" meter, lit from the middle out |
| `rotateVU` | bool | Turn the VU 90 degrees (a vertical bar becomes horizontal) |
| `shareWeatherIP` | bool | The IP and the weather share one row |
| `shareBattRSSI` | bool | The RSSI and battery share one row |
| `rssiDigit` | bool | Show the signal as a number instead of bars |

The five boolean switches are **per layout**, so one conf can have a cramped layout that shares a
row and a roomy one that does not. `false` is the default - writing them out changes nothing, and
they are written out only so that every entry reads the same.

### The two lines

`underLineConf` and `overLineConf` are the same widget drawn at two depths. A page paints its
widgets in the order they were added — `Page::loop()` walks the page's list and each widget's own
`loop()` repaints it — so the under line is added early and the over line is added last on the
player page. Both take their colour from the theme (`theme.line`, the divider's ink), both are a
single filled rectangle, and both exist on the player page only. Two things to know when placing
one: the footer (the volume and buffer bars, and the bottom text row) is a *sub-page* and paints
itself separately, so it can cross the over line; and changing layout at runtime re-creates some
widgets, so an over line that must never be covered is best kept away from areas those widgets use.

---

## 4. What the numbers mean

- **`left` / `top`** are pixels from the top-left corner of the screen.
- **`fontsize` is a multiplier of a 6x8 character cell**, not a point size. So `1` gives a 6x8
  cell, `2` gives 12x16. That is why the confs are full of odd-looking positions.
- **`MAX_WIDTH`** is the usable width inside the border margin, and **`TFT_FRAMEWDT`** is that
  margin — left/right inset in pixels, despite the name. On the 128x64 OLED the margin is 1 px,
  which keeps text off the very first and last pixel column.
- **`width`** in a scroll config is the width of the scrolling window.
- **Scroll speed** is `scrolldelta * 1000 / scrolltime` pixels per second. The shipped confs for
  the 128x64 OLED scroll the title at 50 px/s and the playlist at 150 px/s.

### `FONTSHIFT` and the clock font

`FONTSHIFT` nudges things down to make room for the tall clock font. It is `15` with the default
`CHUNKY6` clock font and `0` when `CLOCKFONT` is `YO_MONO`, and the shipped confs apply it to
`numConf.top`, `clockConf.top` and `vuConf.top` only.

With the default font the clock and the VU sit at `y = 38 + 15 = 53` on a 64 px panel — the last
11 rows. That is worth knowing before you place anything else down there. Whether a given
`FONTSHIFT` looks right is empirical: only a flash proves it.

---

## 5. Turning a widget off

`{ }` means "not used", and it is exactly the same as leaving the line out. Both leave every
field at zero, so the widget is not created.

```c
.title2Conf    = { }, // unused
```

What that looks like per type:

| Type | Off when… |
|---|---|
| `WidgetConfig` | `fontsize` is 0 — **except the clock and the digits**, see below |
| `ScrollConfig` | `buffsize` is 0 (and `fontsize` is 0) |
| `FillConfig` | `height` is 0 |
| `BitrateConfig` | `dimension` is 0 |

Never use a position to decide this: `{ 0, 0 }` is a perfectly valid place to put something.

### The clock and the digits are the exception

`clockConf` and `numConf` ignore `fontsize` completely — they draw with a special clock font, so
a valid clock can have `fontsize = 0`. They count as "present" when **any** of their four fields
is non-zero. Two consequences:

- `{ }` still means "no clock", as you would expect.
- To put a clock at the very top-left corner you must set a fourth field, for example
  `.clockConf = { 0, 0, 0, WA_LEFT }` — otherwise it is indistinguishable from "absent".

### What you cannot leave out

`metaConf` and `playlistConf` are load-bearing: dialogs write into the station line, and the
playlist page is built on the playlist line. Everything else may be omitted.

---

## 6. The VU meter

Three things decide the VU: `vuConf` (where), `bandsConf` (what it looks like) and the two
switches `rotateVU` and `boomboxVU`.

Note that the design of the VU Meter applies directly to the "Bars" and "Digital (LED)" style.
Other VU Styles are derived from this in the code. Design with this in mind.


```c
.vuConf    = { TFT_FRAMEWDT, 38+FONTSHIFT, 1, WA_LEFT },
.bandsConf = { 7, 44, 1, 1, 10 },
.rotateVU  = true,
```

`bandsConf` is `{ width, height, space, vspace, perheight }`:

| Number | Meaning |
|---|---|
| `width` | thickness of **one** channel. Both channels sit side by side, so the real thickness is `width*2 + space`. |
| `height` | length of the bar |
| `space` | gap between the left and right bars |
| `vspace` | gap between the lit segments that make up a bar |
| `perheight` | how many segments you would like the bar to have |

**Work out the footprint before choosing numbers**, because this is the usual way to push a VU
off the screen. With `rotateVU = true` (or any non-zero `align`) the bar runs horizontally:
`height` is its length along x and `width*2 + space` is its height. With `rotateVU = false` and
`align = WA_LEFT` it runs vertically, the other way round.

For the example above the footprint is 15 px tall and 44 px long, placed at `x = 1`,
`y = 38 + FONTSHIFT`.

### Segments: `perheight` is a maximum, not a promise

The segment pitch is `height / perheight`, rounded **down**. So `perheight` is the number of
segments you are asking for, and you usually get slightly fewer: with `height = 44` and
`perheight = 10` the pitch is 4 px, giving 11 segments. The leftover pixels at the loud end are
simply not covered — every segment is the same size and the remainder is wasted.

That is harmless, and there is nothing to work around. If the bar looks more "blocky" than you
wanted, lower `perheight`.

The **speed of the fall is not configurable per layout** — it is worked out from the bar's own
length, so a long bar and a short bar both fall from full to empty in the same time
(`VU_FADE_MS` in `options.h`, one second by default).

### The peak marker

A thin marker that holds the highest recent reading of each channel and then creeps back down.
It is on by default, its colour comes from the theme's `.vupeak`, and it needs **no extra room**
— it is drawn inside the bar's own footprint, at the expense of a pixel or two at the loud end.
A very short bar can therefore look cramped, but it will not overflow.

It also carries over to every orientation for free: turning the bar or switching to the boombox
style moves the marker with it.

---

## 7. Movement, dialogs and `MoveConfig`

`MoveConfig` is `{ x, y, width }` and it means different things depending on `width`:

| `width` | What happens |
|---|---|
| `-1` | **Do not move.** The widget stays where the conf put it. This is what most confs use: `{ 0, 0, -1 }`. |
| `0` or more | The widget travels inside the rectangle while the screensaver is active; `x`/`y` are where it goes and `width` is the width it uses. |
| the whole entry is `{ }` | **"Yield to the VU."** The widget disappears while the VU meter is on screen and comes back when playback stops — see below. |

`weatherMove` is used when no VU is shown and `weatherMoveVU` when one is, so a weather line can
behave differently in the two cases.

### Yielding to the VU

If you want the meter to have the row to itself while it is showing, but still want the clock (or
the weather) when the meter is off, write the move as an empty `{ }`:

```c
.clockMove = { }, // clock disappears while the VU meter is showing
```

That is a real behaviour, not "unused". It is why an empty `MoveConfig` is worth reading twice.

---

## 8. Quirks worth knowing

- **`fontsize` is a 6x8 cell multiplier**, not points.
- **`TFT_FRAMEWDT` is a margin**, not a width, despite the name.
- **`uppercase` does nothing.** Use `PRETEXT_ALLCAPS` in `myoptions.h` instead.
- **On the VU, `align` is an orientation switch**, not text alignment: `WA_LEFT` means vertical,
  anything else means horizontal. If `rotateVU` is set, `align` is ignored for the VU.
- **`clockConf` and `numConf` ignore `fontsize`** and count as present if *any* field is set
  (§5). That is the one place where `{ }` and `{ 0, 0, 0, WA_LEFT }` mean different things.
- **`playlBGConf.height` is only a fallback** — the live playlist row height overrides it
  (11 px per fontsize step).
- **The clock and the VU usually share a band.** Check them against each other before moving
  one of them.
- **Two widgets on one row must be told about it.** If you put the IP and the weather on the same
  `top` line, set `shareWeatherIP`; same idea for `shareBattRSSI` with RSSI and battery. Without
  it they will simply draw over each other.
- **A layout switch does not blank the screen.** Widgets the new layout drops are hidden
  properly, but anything you draw *outside* a widget's own footprint is your responsibility —
  the confs rely on this and it is worth keeping in mind when you place two things very close.

---

## 9. Annotated example: `displayOLED128x64conf.h`

A 128x64 OLED, margin 1, so `MAX_WIDTH` is 126. Top to bottom:

| `y` | What is there | Set by |
|---|---|---|
| 0-17 | title band | `metaBGConfInv` (`height` 17) |
| 1 | station name | `metaConf`, fontsize 2 |
| 19 | title 1 and the bitrate | `title1Conf`, `bitrateConf` |
| 26-38 | playlist highlight | `playlBGConf` |
| 28 | title 2 and the big number | `title2Conf`, `numConf` |
| 30-38 | playlist text, 11 px rows | `playlistConf` |
| 38+FONTSHIFT | the clock, and the VU beside it | `clockConf`, `vuConf` |
| 55 | battery / IP / RSSI / weather | shared bottom row |
| 63 | volume bar, full width, 1 px | `volbarConf` |

The file ships a few variants to experiment with — a plain one, one without a VU at all
(`vuConf` and `bandsConf` empty), and one with a large VU where `clockMove = { }` so the clock
gives up the row while the meter is playing. Read the names in `_layoutNames[]` rather than
expecting a fixed number: the set changes as layouts are tried out.

---

## 10. Checklist before you flash

- `_layoutNames[]` and `_layouts[]` have the same number of entries, in the same order.
- Fields are written in declaration order (§3).
- Every widget you want is actually switched on — check `fontsize` / `buffsize` / `height`.
- Everything you placed fits inside `MAX_WIDTH` and `DSP_HEIGHT`. For the VU, that means
  `width*2 + space` in the thin direction.
- Widgets sharing a line have `shareWeatherIP` / `shareBattRSSI` set.
- `FONTSHIFT` is accounted for on `clockConf`, `numConf` and `vuConf`.
- Build and flash, then look at the real screen — positions near the edges and the exact
  `FONTSHIFT` are the two things that only the hardware can settle.
