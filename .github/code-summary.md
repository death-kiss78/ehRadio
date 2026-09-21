# ehRadio Code Summary (Human + AI Operational Map)

## Mandatory Maintenance Directive

If a change affects code interactions (how files/modules interact), storage keys, WebUI contracts, locale/build/dependency behavior, or other external contracts,`.github/code-summary.md` MUST be updated in the same change set; bug fixes that restore expected behavior are exempt unless they also change code interactions or external behavior/contracts.

This file exists to reduce re-analysis cost for humans and AI agents.

It will get lengthy as more is added to it.

---

## Scope and Intent

This document is intentionally per-file focused for:
- `src/main.cpp`
- `src/core/*`
- `data/www/*`
- `src/locale/*`

Grouped (not one-by-one deep explained) areas:
- `src/displays/display*.cpp/.h` (drivers follow similar shape)
- `src/displays/conf/*.h` (widget placement/config pattern files)
- `src/displays/clockfonts/*` (font assets)

---

## Fast Architecture Overview

### Build-time chain
- `platformio.ini` selects environment, included libraries, and included source files.
- `myoptions.h` selects hardware profile + defaults.
- `src/core/options.h` resolves all defaults/fallbacks and feature flags.
- `.github/workflows/build-release-firmware.yml` verifies generated contributor release artifacts by re-running `builds/fix_releases.py` on CI and diff-checking `builds/releases/` (contains `firmware.txt`, `releases.md`, and `web_assets/`) against what was committed.
- `.github/workflows/build-deploy-page.yml` deploys Pages on `release.published`, and on branch/manual runs it preserves the currently published `firmware-info.json` and manifests instead of overwriting them from `builds/*/web_assets`.
- `.github/workflows/update-timezones.json-automatically.yml` checks out using `DEPLOY_KEY` and pushes over SSH to `dev`, enabling ruleset bypass configured for Deploy keys.

### Runtime chain
- `src/main.cpp` bootstraps system: config -> display -> player -> network -> server/telnet/controls.
- WebUI/WebSocket input path: `netserver` -> `commandhandler` -> `config/player/display/network`.
- State output path: `requestOnChange(...)` in `netserver` -> WebSocket JSON to browser.
- Settings persistence path: `config.saveValue(...)` -> ESP Preferences namespace `"ehradio"`.
- Web-stream resume path: `Config::setLastStationUrl(...)` -> debounced LittleFS file `/data/laststation.url` -> `player.resumeLastWebSource()` for smartstart/reconnect/direct-URL resume.

### Logging chain (serial + telnet)
- `src/core/logging.h` / `src/core/logging.cpp` now define the common log path for runtime diagnostics.
- The public usage pattern remains uppercase macros at call sites, but they now wrap function-backed implementations (`serialLog`, `functionLog`, `bootLog`, `bootLogX`, `errorLog`, `serialLogDot`, `audioLog`) so formatting happens in one backend instead of nested macro layers.
- Core macros:
  - `SERIALLOG(...)`: writes one formatted line to both serial and telnet sinks.
  - `SERIALLOGX(...)`: writes one formatted line to both serial and telnet sinks without line-ending (useful after `BOOTLOGX`).
  - `FUNCTIONLOG(category, ...)`: category-tagged wrapper over `SERIALLOG`.
  - `BOOTLOG(...)` / `BOOTLOGX(...)`: boot-sequence logging helpers. `BOOTLOG` ends with a line-wrapper, `BOOTLOGX` does not.
  - `ERRORLOG(...)`: error-category wrapper.
  - `SERIALLOGDOT()`: progress-dot helper for long-running loops.
  - `AUDIOLOG(category, ...)`: callback-safe logging wrapper for stack-sensitive audio callback contexts.
  - `BOOTLOG_TIME` (defaulted in `options.h` under `ALL_DEBUG_LOGS`, and set in `myoptions.h` for debug builds) makes every `BOOTLOG` line carry the time since boot, zero-padded to five digits, inserted between the 16-character category field and the message: `[BOOT]          00600ms: <message>`. **`logging.cpp` must include `options.h` for this to exist at all**: that header is the only route by which `myoptions.h` reaches a translation unit, so without it the `#ifdef` is false in that one file while the rest of the build has the macro defined — the feature vanishes silently, with a green build and an `#ifdef` that hides its own compile errors. `bootLog()` is the only logger that stamps, selected by category plus newline (`appendNewline && category == "BOOT"`) rather than by an extra parameter, because `bootLogX()` shares the category and differs only in writing no newline — it opens a progress line that `SERIALLOGDOT()` then extends, so a stamp there would land mid-line. The value is `millis()` at emit time and self-widens past 100 s rather than truncating, and telnet receives the same composed line. Boot logs are therefore the boot-timing measurement: consecutive deltas attribute a slow boot to its stages with no temporary instrumentation. **It is also the switch for the three stage timing logs** (`BOOTTIMELOG`, `LITTLEFSTIMELOG`, `CONFIGTIMELOG`): with it undefined their bodies compile to nothing, so a build without it has **no stage lines at all** — turning it off removes the `[BOOT]          00600ms:` prefixes as well, and one `#define` brings both back. Measured: the three bodies and their stamps cost 216 bytes, 52 of them the call sites, which stay in the off build as empty functions so that no translation unit sees a different macro than the rest.
  - `ESPFILEUPDATER_VERBOSE` is derived in `options.h`, immediately after `ESPFILEUPDATER_DEBUG` is settled there — **not** in `logging.h`, where it used to live and where `ESPFILEUPDATER_DEBUG` was only visible when the including file happened to include `options.h` first. Because the library files that include `logging.h` without `options.h` (`FT6336.cpp`, `es8311.cpp`, the four `audioVS1053Ex.cpp` variants) resolved it to `false` while the core resolved it to `true`, one macro carried two values in the same build. All six call sites pass it to `ESPFileUpdater::checkAndUpdate()` and all six include `options.h`; a seventh that forgets would now fail to compile rather than quietly log nothing.
  - Boot stage attribution: `BOOTTIMELOG(name)` closes each block of `setup()` with the cost of the stage that has just finished, e.g. `[BOOT]          01950ms: checkLittleFSandVer                    377ms` — the timestamp is the running total to the end of that stage and the message is that stage's share of it. It exists because the init paths announce completion with `BOOTLOGX` progress lines, which carry no stamp by design (a stamp would land mid-line before the dots), leaving the whole tail of `setup()` unmeasured: about 4.7 s of a 23.4 s boot on the 128x64 VS1053 build. Add a call after every block in `setup()` that can block; the first call measures from power-on. `LITTLEFSTIMELOG` and `CONFIGTIMELOG` do the same for the LittleFS and config paths, and all three live in `logging.cpp` behind declarations and macros in `logging.h` rather than as per-file twins. **Each keeps its own stamp**, so a marker measures against the previous marker of its own kind: the config and LittleFS stages that run *inside* a `setup()` block would otherwise disturb the `setup()` deltas, and a single shared stamp would silently redefine every number in the log. `LITTLEFSTIMELOGRESET()` and `CONFIGTIMELOGRESET()` restart the measurement at the entry to the functions whose first stage would otherwise be measured from the previous marker (`checkLittleFSandVer()`, `Config::init()`, `loadPreferences()`, `initPlaylistMode()`). The functions are declared whatever the build and compile to empty bodies without `BOOTLOG_TIME`, which is why the off build has no stage lines; the `verifyLittleFS` summary line's `, NNNms` is gated the same way.
  - The log write path is shaped by the fact that `Serial` here is USB CDC, not a UART: `HWCDC::write()` takes its TX lock with a timeout, blocks on the ring buffer, then waits 1 ms at a time for the host with no progress, up to its TX timeout (100 ms by default; USBCDC is 250 ms), after which it declares the host gone and sets its `connected` flag false. So one log line could stall the boot for up to a timeout, twice over. Two changes: `emitLogMessage()` now composes the CRLF into the buffer and issues a **single** `Serial.write()`, and `setup()` applies `BOOTLOG_TX_TIMEOUT_MS` (options.h, default 5 ms) through `Serial.setTxTimeoutMs()` under `#if ARDUINO_USB_CDC_ON_BOOT`. Not zero, deliberately: at zero the first failed ring-buffer send gives up immediately and `tries` hits zero, so a burst such as the 40-line config dump comes out truncated rather than merely bounded. Measured cost before the change was about 4.6 ms per line, 186 ms for the config dump alone.
- Contract detail:
  - `Telnet::printf(...)` is telnet-only transport and no longer mirrors to serial.
  - Normal logs no longer route through `Telnet::printf(...)`; `logging.cpp` uses `Telnet::logLine(...)` / `Telnet::logRaw(...)` so the shared logging path avoids the prompt-aware telnet formatter and its extra stack use.
  - Logs should use macros above; direct `Serial.print*`/`telnet.printf` is reserved for explicit transport-specific behavior (for example client-targeted telnet responses and OTA progress streaming).

### Primary shared state objects
- `config` (`Config` singleton): persistent store + station/theme/runtime state.
- `player` (`Player` singleton): audio control and playback state.
- `display` (`Display` singleton): display mode and render queue.
- `network` (`MyNetwork` singleton): connectivity/time/weather state.
- `netserver` (`NetServer` singleton): HTTP/WS server and outbound state queue.

---

## Build and Configuration Files

### `platformio.ini`
- Core environment and dependency declaration.
- Important behavior:
  - `build_src_filter` excludes all by default then re-includes selected folders/files.
  - Board environments add display/audio library includes.
  - `extra_scripts` are used for localization/font replacement and gzip workflow.
- Risk:
  - Wrong env can compile without required modules because files are source-filtered.

### `myoptions.h`
- Board/profile selector and hardware wiring table.
- Sets many user defaults that flow into `config_t` via macros.
- Enables/disables many runtime features by compile-time macro presence.

### `src/core/options.h`
- Canonical fallback defaults and compile flags.
- Includes `myoptions.h` when present.
- **Owns all compile-time guardrails** inline, right next to each respective define.
- Owns shared buffer sizing macros under `/* Maximum lengths of character buffers */`, including `MQTT_URL_SIZE` for stream/artwork URL buffers used by `player`, `audiohandlers`, and MQTT status payload sizing.
- Defines:
  - hardware defaults (pins, feature gates)
  - updater URLs (`FILESURL`, `UPDATEURL`, `CHECKUPDATEURL`) unless disabled
  - weather defaults and thresholds
  - battery defaults/curve/thresholds
  - WebUI and localization defaults
  - curated list defaults
  - **Exception**: locale and language options are handled by locale.h
  - **SPI architecture — Named Bus System** (auto-derived internals — do NOT define `SPI_BUS_SECONDARY`, `SPIA`, or `VS1053_SPIBUS` in `myoptions.h`):
  - **Bus A** = `SPIA` (alias for `&SPI`, the default ESP32 SPI instance). Pins configured by `SPI.begin(SPIA_SCK, SPIA_MISO, SPIA_MOSI)` in `Config::init()` when `SPIA_SCK` is defined; otherwise `SPI.begin()` uses hardware defaults.
  - **Bus B** = `SPIB` (`SPIClass SPIB(SPI_BUS_SECONDARY)` declared and initialized in `config.cpp`). Only exists when `SPIB_SCK` is defined. `SPI_BUS_SECONDARY` uses symbolic constants: `VSPI` (ESP32) / `FSPI` (ESP32-S3/C3) for the primary bus, `HSPI` for the secondary bus on all targets.
  - **Bus pin defines** (set in `myoptions.h`): `SPIA_SCK/MISO/MOSI` manually, or use shorthands: `SPIA_DEFAULT` (chip default pins), `SPIA_DEFAULT_XMISO` (chip default SCK/MOSI, MISO=255 — for display-only Bus A). `SPIB_SCK/MISO/MOSI` manually, or `SPIB_DEFAULT` (chip default secondary-bus pins). `SPI.begin()` / `SPIB.begin()` are only called when the respective SCK is defined and `!= 255`; I2C-only builds skip SPI init entirely.
  - **Per-peripheral bus assignment** (char literals `'A'` or `'B'` — NOT strings): `SD_SPI`, `TS_SPI`, `VS1053_SPI`. `VS1053_SPI` resolves `VS1053_SCK/MISO/MOSI` from the matching bus pins in `options.h` (soft — does not override direct pin defines). SD and TS use the bus *object* directly (`SPIA`/`SPIB`) — no separate SCK/MISO/MOSI derivation needed.
  - **`VSPI FSPI` shim** — defined when target is not ESP32 (`!CONFIG_IDF_TARGET_ESP32`) for third-party library compatibility.
  - **VS1053 bus assignment** — `VS1053_SPIBUS` macro auto-derived in `options.h`. `VS1053_CS != 255` requires `VS1053_SCK` to be set (via `VS1053_SPI` or directly); `#error` if missing. Resolves to `SPIB` if `VS1053_SCK == SPIB_SCK`, otherwise `SPIA`. Used as `&VS1053_SPIBUS` in the `Audio` constructor in `player.cpp`.
  - **SD bus selection** — `SDREALSPI` macro in `sdmanager.cpp`: `SPIB` when `SD_SPI == 'B'` and `SPIB_SCK` is defined, otherwise `SPIA`.
  - **Touchscreen bus selection** — inline in `touchscreen.cpp` `init()`: `SPIB` when `TS_SPI == 'B'` and `SPIB_SCK` is defined; `SPIA` when `TS_SPI == 'A'`; `ts.begin()` (default `&SPI`) otherwise.
- **SD card defines** (set in `myoptions.h`, fallback `255` = disabled in `options.h`):
  - `SD_CS` — chip-select pin; `255` disables SD entirely.
  - `SD_SPI 'A'/'B'` — assigns SD to Bus A or B; SD uses the bus object directly, no per-pin derivation needed.
  - `USE_SD` — feature presence macro, derived from `SD_CS!=255`.
- **I2S internal DAC**:
  - `USE_AUDIO_ESP32_DAC` — defined directly in `myoptions.h` to use the ESP32 internal DAC (ESP32 only, not S3/C3). `I2S_INTERNAL` boolean is removed.
- **Guardrail conventions** (maintain these when adding new options):
  - `#error` for hard-invalid values: wrong board type, mutually exclusive decoders, enum/constant out of range (e.g. `TS_MODEL`, `RTC_MODULE`), bad logical cross-constraints (e.g. `BTN_PRESS_TICKS <= BTN_CLICK_TICKS`, `BATTERY_CRITICAL_THRESHOLD >= BATTERY_LOW_THRESHOLD`).
  - `#warning` + `#undef` for out-of-range tunables in `/* USER DEFAULTS */` section (e.g. `SOUND_VOLUME`, `SCREEN_BRIGHTNESS`): reverts silently to default but now emits a visible warning in the build log.
  - `static_assert` with `__builtin_strcmp` for enumerated string options (e.g. `WEATHER_API`, `WEATHER_WIND_SPEED_UNITS`). **Update the `static_assert` whenever a new provider/value is added.**
  - `/* PREVENT BOARD-DEFINED PIN RE-USE */` section lives after the `/* ESP DEVBOARD */` LED block (requires `LED_PIN` and `ESP_S3C3` to be defined first). Covers LED vs RST pin conflicts only — keep it narrowly scoped.
- **What is intentionally NOT guarded**: booleans (compiler error is obvious), pin numbers (board-dependent range), free-form strings (`AP_SSID`, `MQTT_*`, URLs), color macros (R,G,B triplets), `AUTOBACKLIGHT(x)` (C macro function), `BATTERY_CURVE_MV/PCT` (already has `static_assert` in `battery.cpp`).
- **PSRAM buffer sizing** — `PSRAM_BUFSIZE` macro controls the audio input buffer and FLAC reserved buffer sizes. Defaults differ by board
- Buffer bar visual mapping:
  - `BUFFERBAR_VISUAL_FULL_PERCENT` controls where input-buffer fill is rendered as visually full.
  - default `82` means 82% raw fill maps to 100% bar width; set `100` to keep direct 1:1 mapping.

## Compile-Time Modularity and Build Variants (`#if` / `#ifdef` behavior)

This codebase is strongly compile-time modular. Runtime behavior can differ significantly even with the same source, depending on the selected PlatformIO environment and macro definitions.

### Where behavior is selected
- `platformio.ini`:
  - determines which board/env is built
  - controls source inclusion via `build_src_filter`
  - injects build flags that enable/disable subsystems
- `myoptions.h`:
  - hardware profile pins and feature toggles
  - indirectly controls which code paths in `src/core/*` and `src/displays/*` compile
- `src/core/options.h`:
  - fallback defaults and many `#ifndef` guards
  - central place where missing user macros are filled

### What commonly changes between builds
- Audio backend and related controls:
  - I2S audio path vs VS1053 path are mutually exclusive — enforced via `#error` in `options.h`.
- Display backend:
  - selected display model changes driver implementation and capabilities.
  - **Resolution/interface system**: `DSP_MODEL` = controller chip, `DSP_WIDTH`/`DSP_HEIGHT` = panel resolution, SPI or I2C type may be detected by `I2C_SDA` and `I2C_SCL` pins defines. Resolution defaults per DSP_MODEL in `options.h`, overridable in `myoptions.h`.
  - **dspcore.h**: one `#elif` per DSP_MODEL, sets feature flags (`PSFBUFFER`, `DSP_OLED`, `DSP_LCD`).
  - **dspfont.h** (new): selects bootlogo, clock font, TIME_SIZE by resolution.
  - **dspconf.h** (new): selects `conf/display*conf.h` by resolution × display category.
  - Display `.h` files now delegate conf/font/bootlogo to these central files — no per-file branches for resolution.
  - Conf files no longer define DSP_WIDTH/DSP_HEIGHT (set upstream in options.h).
  - **Removed enum values** (collapsed into resolution variants): DSP_ST7789_240, DSP_ST7789_76, DSP_SSD1306x32, DSP_SSD1305I2C, DSP_1602I2C, DSP_2004I2C, DSP_SSD1327_64. I2C variants detected by `I2C_SDA` and `I2C_SCL`.
  - ST7735 DTYPE still required for library; resolution auto-derived from DTYPE in displayST7735.h.
- Network/update features:
  - some online update and service behavior is compiled out by feature flags.
- MQTT, touch, RTC, SD, battery helper behavior:
  - each has compile gates that can remove handlers/routes or no-op logic.

### Build-variant risk pattern
- A fix validated in one env may not compile or behave in another env because:
  - different source files are included
  - different `#ifdef` branches are active
  - defaults from `options.h` may mask missing `myoptions.h` values

### Practical checklist before merging a change
1. Confirm which env(s) the change targets in `platformio.ini`.
2. Check affected macro guards in touched files.
3. Verify any new setting has safe defaults in `options.h`.
4. Ensure mutually-exclusive hardware blocks still compile (audio/display especially).
5. If possible, do at least one alternate-env compile sanity check.

---

## Boot and Control Flow

### `src/main.cpp`
- `setup()` major sequence:
  1. serial + LED + RGB + battery init
  2. `config.init()`
  3. `backlightControls.init()`
  4. `display.init()`
  5. `player.init()`
  6. `battery.bootStatus()`
  7. `network.begin()`
  8. if no connectivity: start minimal server + controls + display start and return
  9. if connectivity:
     - `config.initPlaylistMode()`
     - `netserver.begin()`
     - `telnet.begin()`
     - controls init
     - display start
     - optional MQTT init
     - optional smart-start playback
     - `startup.startupServices()`
     - `netserver.setBootReady(true)` only after setup work is actually complete
- `loop()`:
  - AP mode: Improv + captive DNS
  - normal: telnet loop
  - RGB loop
  - `battery.loop()` + `battery.applyPowerPolicy()`
  - player loop (connected/SD ready)
  - controls loop

---

## Core Folder Per-File Map (`src/core`)

### Module Convention
All modules in `src/core/` follow the **class + global instance** pattern:
- The header declares a `class Foo` with the full public interface and private members/methods.
- The `.cpp` defines all `Foo::` methods and declares the single global instance: `Foo foo;`
- The header provides `extern Foo foo;` so callers can use `foo.method()`.
- Hardware-conditional modules use a real class in the `#if` branch and a no-op stub class in the `#else` branch; `extern Foo foo;` is placed after the guard.
- **C-style free-function modules are not acceptable in `src/core/`.**

### Naming Style
- **camelCase** for all identifiers: private members, local variables, private methods (e.g. `inferredCharging`, `lastVoltageMv`, `readAndUpdate`).
- No underscore-prefixed names (e.g. `_myVar`, `_myMethod`) — use plain camelCase instead.
- Public API methods follow existing verb-noun camelCase: `init()`, `getStatus()`, `setEncAcceleration()`.
- File-scope `static const` constants also use camelCase (e.g. `pctSampleMax`, `emaAlphaQ`).
- `ALL_CAPS` applies only to `#define` macros and hardware pin constants inherited from the config cascade.

## `src/core/common.h`
- Shared enums and structs used across modules (display modes, requests, control events, etc.).
- Coupling:
  - Imported by display, controls, and command paths.

## `src/core/options.h`
- See earlier build section.
- `DSP_INVERT_TITLE` macro removed — replaced by runtime `config.store.inverttitle` boolean.
- `DSP_TFT` added to `dspcore.h` for all TFT display models — used to gate color-theme reloading (monochrome displays skip it).
- VU visualiser tunables live here: `VU_REFRESH_MS`, `VU_FADE_MS`, `VU_PEAK_FREEZE_MS`, `VU_PEAK_FADE_DIV`, `VU_PEAK_THICKNESS_MILLI`, `VU_SPECTRUM_MIN_PX`, `VU_SPECTRUM_SPACE_PX`, `VU_SPECTRUM_DB_FLOOR`, `VU_SPECTRUM_MAX_CHANNELS`, `VU_HISTORY_MIN_PX`, `VU_CAPTURE_SAMPLES`, `VU_DUTY_FACTOR` and `VU_STYLE_DEFAULT`. **`VU_CAPTURE_SAMPLES` must stay a power of two** — it is both the rolling sample window and the FFT size. `VU_REFRESH_MS` is a *floor* on the redraw interval, i.e. the ceiling on the frame rate, and `VU_DUTY_FACTOR` stretches that interval by the frame's measured draw cost (see the VU Widget Rendering section).

## `src/core/config.h`
- Defines persistent struct `config_t store`.
- `theme_t` no longer includes `theme.heap`; runtime input-buffer rendering uses `theme.buffer`.
- New fields: `uint8_t themeId`, `bool inverttitle`. `color565()` method removed.
- Defines station/theme structs and config API.
- Defines key constants for LittleFS paths and data file locations.
- `theme_t` now includes screensaver-specific clock palette fields (`clockss`, `clockbgss`, `secondsss`, `dowss`, `datess`) so screensaver clock/date elements can use colors independent from normal PLAYER-mode clock colors.
- `station_t` fields (`name`, `url`, `title`) are sized by `STATION_FIELD_LENGTH` (default 170, defined in `options.h`). These are RAM-only fields — not NVS-stored. `BUFLEN` has been retired; use `STATION_FIELD_LENGTH` for station metadata buffers across the codebase.
- `SD_PATH_LENGTH` (256, defined in `sdmanager.h`) is used for SD filesystem path buffers where paths may exceed 170 bytes.
- `Config::keyMap` declaration controls Preferences key mapping.
- IR remote codes use a separate named store: `struct irstore_t` with one `uint64_t[3]` field per button (`power`, `mute`, `up`, `down`, `prev`, `next`, `play`, `mode`, `hash`, `n0`…`n9`). It is persisted in its own NVS namespace (`ehradioir`) through a dedicated key map in `config.cpp`; buttons are addressed by name, never by array position. The old positional `ircodes_t` blob was removed.
- `Config::saveValue(...)` API now has two simple overloads only:
  - typed: `saveValue(T* field, const T& value)`
  - string: `saveValue(char* field, const char* value)`
- `Config` also owns a separate RAM-backed `lastStationUrl` resume buffer that is intentionally *not* part of `config_t` / Preferences; it is persisted through `/data/laststation.url` with a dedicated debounce path because previews/direct URLs can change more often than normal prefs.
- Legacy compatibility parameters (`commit`, `force`, and string `size_t N`) were removed.
- String saves now normalize into a zero-filled fixed-size buffer before compare/write to avoid reading beyond short source strings.
- Both overloads share a single internal write-if-changed path (`missing key` OR `size mismatch` OR `content changed`) before calling `prefs.putBytes(...)`.
- Save-path writes now emit telnet+serial config logs by key name; sensitive keys (`mqttpass`, `weatherkey`) are masked as `*`.

## `src/core/config.cpp`
- Persistent storage/defaults/hardware bootstrap center.
- Main responsibilities:
  - load and validate Preferences (`cfgset` marker)
  - defaults/init logic
  - LittleFS mount and Config-owned required-file checks used during playlist-mode initialization
  - version marker management (`/data/ehradio.ver`)
  - load/save the debounced Web-stream resume hint (`/data/laststation.url`)
  - playlist-mode initialization and file-presence checks before delegating playlist indexing/load helpers to `utility`
  - canonical LittleFS asset allowlists (`Config::wwwFiles[]`, `Config::dataFiles[]`) used by startup recovery and file-maintenance flows
  - reset section handlers (`defaultSettings(...)`)
  - named IR code storage in the dedicated `ehradioir` NVS namespace (`IR_MAGIC` 1812 stored under key `irset`, one key per button via `irKeyMap[]`); helpers `loadIR()`, `saveIR()` / `saveIR(button)`, `irCodes()`, `clearIR()`, `clearDuplicateIR()`, `irButtonByName()`, `irButtonCount()`, `irButtonKey()`, `irAction()`
  - `deleteOldKeys()` also drops the legacy `ircodes` key from the `ehradio` namespace
- SPI bus initialization: `Config::init()` calls `SPI.begin(SPIA_SCK, SPIA_MISO, SPIA_MOSI)` only when `SPIA_SCK` is defined and `!= 255`, and `SPIB.begin(SPIB_SCK, SPIB_MISO, SPIB_MOSI)` only when `SPIB_SCK` is defined and `!= 255`. I2C-only builds skip SPI init entirely. Both buses are initialized before `_initHW()` and before `display.init()` / `player.init()`. Both SPI buses are fully configured before any peripheral uses them. `SPIClass SPIB(SPI_BUS_SECONDARY)` is declared at file scope in `config.cpp`; extern declared in `config.h`.
- **A safe-mode boot pays a deliberate 1 s before anything else.** `setup()` holds `if (!config.store.bootStableMarker) delay(1000)` and then dumps the config (`bootInfo`, ~196 ms at 115200), so the early stage measures ~1586 ms in safe mode against ~590 ms settled - the NVS path itself costs ~412 ms and needs no optimisation. `bootStableMarker` is only set ~10 s after the startup services finish, so rebooting the device within ~20 s of a boot puts the next one into safe mode with that delay (worth knowing when timing boots).
- SD-specific behavior:
  - `_initHW()` configures `SD_CARD_DETECT_PIN` as `INPUT_PULLUP` when available
  - `initPlaylistMode()` short-circuits to `PM_WEB` without calling `sdman.start()` when `SD_CARD_DETECT_PIN` reports slot-empty during SD boot
  - `changeMode()` short-circuits SD mode switches the same way, avoiding the SPI retry path when the slot is empty
- Key interaction:
  - almost every module reads/writes through `config`.
- Theme loading note:
  - `Config::loadTheme()` now uses `memcpy_P(&theme, &_themes[config.store.themeId], sizeof(ThemeData))` — loads from PROGMEM `_themes[]` array at runtime. No longer uses compile-time `COLOR_*` macros.
  - `applyInvertTitle()` removed from Config (moved to Display).

## `src/core/startup.h` / `startup.cpp`
- Boot-only orchestration module following the standard core `class + global instance` pattern (`Startup startup;`).
- **Boot stability is a state, not a timer.** `Startup::_services` is `SVC_NONE` / `SVC_WILL_RUN` / `SVC_DONE`, and `setup()` has already settled which one applies by the time `loop()` can run, because `startupServices()` has exactly one call site — [`main.cpp:106`](src/main.cpp:106), inside `setup()`. `loop()` then: for `SVC_NONE` proves the boot over `BOOT_STABLE_TIME` from power-on (nothing risky will run, but an early crash must still trip Safe Mode on the next boot), waits indefinitely for `SVC_WILL_RUN`, and marks stable `BOOT_STABLE_TIME` after `SVC_DONE`. `SVC_NONE` is the **only** case where the power-on count is used, and that is what makes it safe — the power-on count is wrong only when it is applied while the state is still unknown.
- **The three boot outcomes, and why the state can always be settled before `loop()`:** *SD offline* (`network.offlineMode || config.store.SDoffline`) never calls `checkSafeMode()`, so `_bootStablePending` stays false and the marker is not touched at all — that is the long-standing workaround and it is deliberate. *No WiFi / soft AP* returns early from `setup()` before the services call, leaving `SVC_NONE`, and the boot then proves itself over `BOOT_STABLE_TIME` from power-on. *Connected* records `SVC_WILL_RUN` before the task is created, and the task sets `SVC_DONE`, so a download that finishes quickly cannot be missed.
- **SD playback mode is deliberately left waiting.** The services task parks in `while (config.getMode() == PM_SDCARD)` until the user leaves SD, so the boot stays unproven until the downloads actually happen. The known consequence: a device that boots into SD playback and is powered off without ever leaving SD never proves its boot, and the next boot comes up in Safe Mode once (smartstart and autoupdate off for that session). Chosen deliberately over marking it stable, because the services are the risk being guarded.
- **What replaced what:** the first attempt made `_servicesDoneMs == 0` fall back to the power-on count, which marked the boot stable at exactly the moment the services were starting (`[BOOT] Boot stable after 10022 ms (startup services did not run)` while `[Services] Startup Async Services starting` was on the same second). The second attempt kept a `BOOT_STABLE_BACKSTOP_MULT` timer as a safety net, which allowed a boot to be called stable while the risky window was still open. Both are gone.
- The startup services are a known high-risk path: three concurrent TLS sessions against ~75 KB of internal heap, which has been observed to exhaust it and crash the boot. See `.github/code-issues.md` section 7.
- Owns startup-time helpers that were previously mixed into `config.cpp`:
  - boot-time version marker and required LittleFS/WebUI file verification (`checkLittleFSandVer()`) — the verification itself lives in `Utility::verifyLittleFS()`, see the LittleFS notes below
  - **The partition-size lookup penalty belonged to SPIFFS, and the migration removed it. This supersedes the old "prefer the 8 MB table" conclusion.** The numbers recorded here were measured under **SPIFFS** on the sh1106_vs1053_3buttons build: a missing-name probe cost ~187 ms on the 3.38 MB partition in `builds/partitions/default_16MB.csv` and ~87 ms on the 1.5 MB one in `default_8MB.csv`, `SPIFFS.begin()` mount moved 229 → 103 ms, and the `netserver.begin()` file cache 797 → 382 ms — the same 2.25x factor, three independent measurements. SPIFFS proved a name existed, or did not, by walking the partition's lookup structures, and that is why the conclusion then was that the **8 MB table was the layout to prefer**. LittleFS does not work that way: it resolves a name by walking the directory's entry chain and mounts from a superblock, neither of which scales with partition size — and the measured mount win after the migration (229 → 27 ms) is precisely that change. **Partition table choice is therefore no longer a filesystem-speed decision, and shrinking the partition buys nothing.** What partition size still governs under LittleFS is free-block headroom and wear-rotation depth: 1.5 MB is ~384 blocks of 4 KB and 3.38 MB is ~864, so the larger table gives the allocator more places to rotate writes and more room for the ~300 KB peak demand described in `code-issues.md` section 3.2. A larger filesystem is therefore mildly better for long-term wear, not worse, and the only size-sensitive call left is space accounting — `usedBytes()` and `totalBytes()` come from a filesystem traversal rather than a constant-time counter, so measure rather than assume if that ever matters.
  - **Web-file verification is one listing pass, not N lookups.** `Utility::verifyLittleFS()` in `utility.cpp` lists `/www` once, matching every listed name against `Config::wwwFiles` as it goes, so it stores no names at all — only two bitmasks (plain seen, `.gz` seen) — and returns whether each required file is present in either form. Its single write is the duplicate rule: a plain file whose `.gz` twin exists is removed. It replaced two identical per-file probe loops (`requiredWebFilesExist()` in `startup.cpp` and `Config::_wwwFilesExist()`), which together cost **7.48 s of a 23.2 s boot** — 33 lookups each, run twice, because the second one re-answered a question `checkLittleFSandVer()` had already answered. `config.wwwFilesExist` is now computed once there and reused by `initPlaylistMode()`.
  - **The filesystem is LittleFS, and four of its differences from SPIFFS are load-bearing** (migrated from SPIFFS; full reasoning in `plans/spiffs-to-littlefs.md`):
    - **The partition label is `littlefs` but the partition SubType stays `spiffs`.** LittleFS resolves its partition through `esp_vfs_littlefs_register()`'s `partition_label`, so `FS_PARTITION_LABEL` / `FS_MOUNT_POINT` in `config.h` are passed explicitly to `LittleFS.begin()` — the Arduino default is still the legacy label `spiffs`. The CSV SubType cannot follow: `gen_esp32part.py` rejects `littlefs`, `esp_partition_subtype_t` has no `LITTLEFS` value, and PlatformIO matches only the literal strings `spiffs`/`fat`/`littlefs` when locating the FS offset for `buildfs`/`uploadfs`. Keeping `spiffs` (0x82) also keeps `Update.begin(size, U_SPIFFS)` resolving, so the filesystem-image upload needs no custom `esp_partition` code.
    - **Directories are real, so `/www` and `/data` must exist.** `checkLittleFSandVer()` calls `LittleFS.mkdir("/www")` and `mkdir("/data")` on the common path taken by both a successful mount and a fresh format, because every `/data/*` write (playlist, wifi, version marker) and every `/www/*` write needs its parent to exist. SPIFFS inferred the directory from a path prefix; LittleFS does not. The built image happens to supply `/data` (the source tree ships `data/data/wifi.csv` and `playlist.csv`), but the `format` command erases both directories, hence the mkdir.
    - **A root listing yields directory entries, not files.** `LittleFS.open("/")` returns only `www` and `data`, so `verifyLittleFS()` lists `/www` directly and `pruneLittleFS()` walks `/www` and `/data` explicitly, comparing basenames. The old "walk `/` once" shape would have reported every www file missing on every boot, and would have pruned nothing.
    - **`rename()` no longer overwrites an existing target.** Every call site removes the destination first (`importWifi()`/`saveWifi()` and `cleanPlaylist()` in `utility.cpp`), and `ESPFileUpdater` does `exists`→`remove`→`rename` internally, so the invariant holds — but any new rename must preserve it.
    - Names that changed with it: `clearspiffs` → `clearfs`, the `updatetarget` value `spiffs` → `littlefs`, `checkSpiffsandVer`/`verifySpiffs`/`pruneSpiffs` → `checkLittleFSandVer`/`verifyLittleFS`/`pruneLittleFS`, `SPIFFSTIMELOG` → `LITTLEFSTIMELOG`. The emergency form in `netserver.h` deliberately stays **firmware-only** and keeps its hidden `updatetarget=fw`: the `/update` handler can also target the FS partition via `U_SPIFFS`, but that writes raw flash under a mounted LittleFS with no `LittleFS.end()` first and no image-size or partition-table validation, so it is not offered on a recovery page. The hidden field also matters because `getParam()` returns nullptr when the parameter is absent, which is why that dereference is now guarded. Build side, `board_build.filesystem = littlefs` selects `mklittlefs`, and both gzip `extra_scripts` key off `$BUILD_DIR/littlefs.bin` — a stale `spiffs.bin` reference there silently disables the compress/restore step.
    - **There is no SPIFFS → LittleFS upgrade path.** A full flash (bootloader + partitions + firmware) is required; an OTA firmware-only update leaves an unmountable partition because the in-flash partition table still carries the old label. Saved `playlist.csv` / `wifi.csv` do not survive.
    - **Boot-time FS cost, and the two fixes that followed the measurement.** Measured on the SH1106/VS1053 build, LittleFS is 8.5x faster to mount (229 → 27 ms) and much faster on lookup-miss-heavy paths, but roughly 4–25x more expensive per small file operation, so total boot regressed 15402 → 15747 ms. Two changes claw that back, and both help under either filesystem: `StaticFileCache::loadOne()` no longer calls `exists()` before `open()` (open() is the existence test, and a `size > 0` guard covers the core quirk the FS health check documents), and the FS health check now runs only when `bootStableMarker` says the previous boot did **not** prove stable — the same condition `checkSafeMode()` uses to enter Safe Mode. The single biggest regression was `netserver.begin` (the 17-file PSRAM cache load) at 798 → 1284 ms.
    - **The verify pass and the update churn are deliberate, not oversights.** `checkLittleFSandVer()` lists all 17 www files via `verifyLittleFS()`, and then `netserver.begin()` opens them again in `loadAll()`. The second pass is knowingly kept: presence must be known at `main.cpp:65`, before `initPlaylistMode()` reads `config.wwwFilesExist` at `main.cpp:100` and before `loadAll()` runs at `main.cpp:102`, and the early `pruneLittleFS()` / version-marker write inside `checkLittleFSandVer()` depends on it as well. Folding the two passes would mean reordering boot stages across `main.cpp`, `startup.cpp`, `config.cpp` and `netserver.cpp` — reviewed and declined. Cost is 385 ms under SPIFFS and 422–555 ms under LittleFS.
    - **`ESPFileUpdater`'s `.meta` sidecars and the churn they cause are load-bearing.** Each updated file costs a `.tmp` create plus appends, then `exists`→`remove`→`rename`, then a `writeMeta()` of `localPath + ".meta"` — and the `NOT_MODIFIED` path still rewrites that `.meta` just to bump its timestamp, so **every boot rewrites one `.meta` per checked URL even when nothing changed**. That is the intended mechanism for deciding whether a download is needed, so it stays. LittleFS pays it synchronously (a metadata commit, often with a block erase) where SPIFFS deferred the cost to GC — which is the whole of the perceived "slower file operations" difference.
    - **`pruneLittleFS()` is an update-scoped reset, not a periodic cleaner — and deleting the `.meta` sidecars is part of that by design.** Its trigger is `!config.wwwFilesExist || !LittleFS.exists(VERSION_PATH)` at `startup.cpp:248`, so it runs in exactly three situations: a **firmware version change** (a mismatch sets `wwwFilesExist = false`), a **new install** (no version file present), or **a missing www file while the version still matches** (the version-match branch sets `wwwFilesExist = verifyLittleFS()`, so one absent asset alone is enough). It never runs on a healthy boot — all 17 files verify present and the version file exists, making the condition false, which the boot logs confirm. Because the version marker is rewritten on every prune, the version-change case fires once per firmware change and not repeatedly. Clearing everything derived at that moment is deliberate: besides the `.meta` sidecars it also removes `/data/index.dat`, `/www/searchresults.json`, `/www/search.txt`, `/www/curated.json`, `/www/pl_import.json` and `/data/new_ver.txt`, all of which are rebuildable — so a firmware change starts from a clean slate instead of mixing stale bookkeeping with new assets. The only cost is that intact assets get re-fetched on that one boot. The third trigger is self-limiting for the same reason: prune rewrites the marker with the same version, the files come back, and the next boot verifies clean.
    - **LittleFS has no fsck and needs none, and its one maintenance knob is unreachable.** Consistency is resolved atomically at mount (the force-consistency step inside `LittleFS.begin()`), and reclamation is continuous, so there is no repair or defrag pass to schedule — which is precisely why `begin(false)` falling back to `begin(true)` is the complete recovery path, and why gating the read/write health check behind `bootStableMarker` is safe. The closest thing the design has to periodic self-maintenance is `block_cycles`, the wear-levelling relocation threshold, and it cannot be set from here: arduino-esp32 3.2's `LittleFSFS::begin()` exposes only `formatOnFail`, `basePath`, `maxOpenFiles` and `partitionLabel`. The real ongoing risk is **free space**, not corruption — see `.github/code-issues.md` section 3.2.
  - **`pruneLittleFS()` is a repair, not a verifier, and must never run on the boot path.** It deletes everything outside the `wwwFiles`/`dataFiles` keep lists — which includes `/www/searchresults.json`, `/www/search.txt`, `/www/curated.json`, `/www/pl_import.json` and `/data/new_ver.txt` — so it stays on the missing-files and version-mismatch paths (and the `clearfs` command). It was formerly `cleanupSpiffs()`, renamed so the pair reads as what they are: one verifies, one prunes.
  - loading saved SSIDs from `/data/wifi.csv` into `config.ssids`
  - stale search-result cleanup under `/www/searchresults.*`
  - required WebUI asset download and recovery flow
  - version-file parsing for online-update detection
  - startup background update scheduling (`startupServicesAsync`) — spawned as a FreeRTOS task on `NETWORK_CORE` at low priority:
    - waits `STARTUP_SERVICES_DELAY` seconds before any work, letting audio buffer fill first
    - verifies WebUI locale JSON file; downloads if missing
    - checks for new firmware version via `new_ver.txt`; triggers OTA if `autoupdate` is enabled
    - downloads default `playlist.csv` from `PLAYLIST_DEFAULT_URL` if file is missing
    - updates `timezones.json.gz` and `rb_srvrs.json` from online sources
    - cleans stale search results older than 24 hours
    - deletes the `ESPFileUpdater` param and self-terminates via `vTaskDelete(NULL)`
  - `deassertCsPins()` — called from `main.cpp` `setup()` before any device init. Sets all known SPI CS pins (`VS1053_CS`, `SD_CS`, `TFT_CS`, `TS_CS`) to `OUTPUT` + `HIGH` to prevent floating CS from causing bus contention during peripheral detection.
  - safe mode boot crash-loop detection (`checkSafeMode`, `markBootStable`, `loop`): reads NVS key `bootstablemark` at boot — if the previous boot did not complete successfully it sets `Startup::_safeMode` for this session, which suppresses the automatic version-check/autoupdate and the automatic smartstart playback, so the device does not auto-reconnect to a crash-causing stream. The stored `smartstart`/`autoupdate` values are deliberately **not** overwritten: they feed the WebUI (`GETCONTROLS`, `GETSYSTEM`) and `Utility::turnoff()` writes `smartstart` back to NVS, so an in-memory override would both misreport the settings and persist itself. A manual `turnon` is not suppressed. **Note the marker is now set by the state machine above, not by a fixed uptime**: `BOOT_STABLE_TIME` after the startup services finish when they will run, or over `BOOT_STABLE_TIME` from power-on when they will not run.
  - `icon()` — the boot-mode glyph for the boot dots line: the SD pair (`\030\031`) when `network.offlineMode` or `SDoffline`, PAUSE (`\034`) when the previous boot never proved itself, PLAY (`\035`) for smart start, VOL_75 (`\026`) otherwise. The boot screen is its **only** consumer, and the sampling rule still applies: `Display::_bootScreen()` reads it while the screen is built, because `display.init()` in `setup()` runs *before* `checkSafeMode()`, which is what clears `bootStableMarker` — a later read would report PAUSE on every boot. The widget keeps the returned literal for the session, which is why the glyphs here must stay string literals.
- Coupling:
  - drives `utility` for shared update/download helpers
  - reads Config-owned asset allowlists during required-file recovery
  - updates `netserver.newVersion` / `newVersionAvailable`
  - stops playback and drives `display` during required-file recovery
  - `main.cpp` calls `startup.startupServices()` after network/server startup
  - `network.cpp` calls `startup.initNetwork()` during WiFi credential load

## `src/core/network.h` / `network.cpp`
- `network.h` declares `MyNetwork` state and API; states: `CONNECTED`, `SOFT_AP`, `FAILED`, `SDREADY`.
- Connectivity, periodic scheduling (`ticks`), weather provider logic, and time sync.
- Main responsibilities:
  - STA connect + optional strongest RSSI/BSSID mode
  - AP fallback with DNS captive portal
  - Improv provisioning flow
  - WiFi reconnect/disconnect handlers
  - periodic ticker logic for:
    - time sync interval
    - weather sync interval
    - screensaver timing
    - RSSI updates
    - SD card hot-insert detection (when `SD_AUTOPLAY && SD_CARD_DETECT_PIN!=255`): polls `SD_CARD_DETECT_PIN` every ~2 s in `divrssi` block; calls `config.changeMode(PM_SDCARD)` on insertion
  - weather provider dispatch:
    - `OM1` Open-Meteo
    - `OW25` OpenWeather 2.5
    - `OW30` OpenWeather 3.0
  - weather cache and formatting logic
  - centralized runtime logging for reconnect/weather/boot progress/time-sync via `FUNCTIONLOG`/`SERIALLOG`/`BOOTLOGX`
  - web-stream reconnect now resumes through `player.resumeLastWebSource()` so direct URL sources can recover via `/data/laststation.url` instead of always falling back to `lastStation`
- **The boot Wi-Fi path (`network.begin()` → `wifiBegin()`).** With `config.store.wifiscanbest` it runs one synchronous `WiFi.scanNetworks()` and orders the candidates by RSSI, pinning the BSSID; otherwise it walks the saved SSIDs in order. Each candidate gets a **time-based** ceiling of `WIFI_ATTEMPTS * 500` ms (8 s) while polling `WiFi.status()` every `WIFI_CONNECT_POLL_MS` and printing a dot. That ceiling has to cover association **and** the lease, because `WL_CONNECTED` is raised only on `ARDUINO_EVENT_WIFI_STA_GOT_IP` — 0.5 s or 4.8 s after association on this hardware. Association is therefore logged separately, from `WiFi.RSSI()` going non-zero (`Associated after Nms (RSSI x) - waiting for the address`), and a candidate that hits the ceiling is retried **once** in place (`WiFi.disconnect(false, false)`, wait for RSSI to clear up to `WIFI_SETTLE_MS`, `WiFi.begin()` again) before it is abandoned. Exhausting the list returns false and `network.begin()` raises the SoftAP.
  - **What the scan costs.** `WIFI_SCAN_DWELL_MS` (options.h, default **120**) is passed as `max_ms_per_chan`, but `WiFiScan.cpp` hardcodes `scan_time.active.min = 100`, so it is a **ceiling**: per-channel cost is dominated by fixed overhead (~320 ms of it, ~441 ms per channel measured at 120), which makes a 13-channel scan ~5.7 s. `wifiBegin()` scans a **second** time at the IDF default 300 only when the configured dwell finds nothing, so the old behaviour stays the last resort and only a failure there reaches the SoftAP — which is what makes a lower dwell safe.
  - **`WIFI_CONNECT_POLL_MS`** (options.h, default **100**) replaces the old hardcoded 500 ms poll, which quantised every join measurement to half a second. The per-candidate ceiling is time-based rather than attempt-based precisely so a shorter poll cannot silently shrink the 8 s budget.
  - `WiFi.setSleep(false)` is applied in `setWifiParams()` **after** a successful connect; nothing clears the modem-sleep policy before a connect.
  - The `SCANNINGWIFI` message is sent with `display.putRequestDelayed(..., DSP_BOOTMSG_DELAY_MS)` (**2000** ms) so the firmware version on the boot line stays readable instead of being replaced the moment the scan starts.
  - **WARNING - do not re-add an asynchronous scan.** `WiFi.scanNetworks(true)` returns `WIFI_SCAN_FAILED` (-2) after 2.5-6 s on this build and leaves the radio unable to scan at all afterwards: every later synchronous scan returns 0 networks in 2-12 ms, and `scanDelete()` does not clear it, so the fallback cannot recover and the boot lands in the SoftAP. Three variants failed identically - no driver call during the scan, dwell 120 or 300, and leaving the modem-sleep policy alone. The ~860 ms it would save is not worth a radio that stops scanning.
  - **WARNING - a pinned static address was tried on hardware and removed; do not re-attempt it.** The feature (an `SMART_STATIC_IP` `#ifdef` in `network.cpp`) remembered the DHCP lease once a boot had proved itself, applied it before the join and judged the join on the wire instead of waiting for `WL_CONNECTED`. It worked and it was fast (~4.6-4.8 s to Ready against ~8.7 s on plain DHCP), but **this stack cannot prove an address is free**: lwIP here has no ACD (`LWIP_ACD` / `LWIP_DHCP_DOES_ACD_CHECK` are absent from every target's `sdkconfig`, so it cannot be switched on without rebuilding the prebuilt IDF libraries), and the hand-rolled ARP probe is defeated by lwIP itself — **an ARP reply whose sender IP equals the netif's own address is treated as us**, so no foreign entry is ever cached under our address and `etharp_find_addr()` can never see the competing host. With a PC holding that exact address, first manually and then as a DHCP reservation, the radio still logged "nobody else holds it" and used it. What the probe does prove is that the address is bound and the gateway answers; it does not prove uniqueness, and the failure mode is a **silent LAN collision**, not a slow boot. An address is only safe as a router reservation by MAC or one outside the DHCP pool, and the firmware cannot verify that — which is why it is gone rather than shipped with a caveat. Facts the work left behind: `WL_CONNECTED` is set in exactly one place, on `ARDUINO_EVENT_WIFI_STA_GOT_IP` (`WiFiGeneric.cpp:1095`); a `WiFi.config()` issued before the connect only fills esp_netif's *stored* copy while the lwIP netif still reads `0.0.0.0`; and a **down** netif cannot transmit at all (`etharp_request()` returning `ERR_OK` means "accepted for transmission", not "sent"). Also not to be re-tried: re-applying the address after association, "kicking" the DHCP client to raise the netif, and priming every connect attempt.
  - **The join budget: two attempts per candidate, and the second is twice as big.** `WIFI_ATTEMPTS * 500` = 8 s has to cover association *and* the lease, because `WL_CONNECTED` is GOT_IP and the late-GOT_IP mode measures 4.8 s on its own — that mode is one lost DISCOVER, as lwIP retransmits at about +0, +4 and +12 s, so a window can be spent on nothing but lost DISCOVERs and still be called a failure. Each candidate therefore gets exactly two windows: 8 s as before, then a retry of `WIFI_ATTEMPTS * 500 * WIFI_RETRY_SCALE` (options.h, default **WIFI_RETRY_SCALE 2** = 16 s). Only after the second failure is the next candidate tried, and only after that the SoftAP — worst case ~25 s per candidate against ~17 s before. The retry is a **real restart, not a re-association**: the shared helper `wifiRestartForRetry()` does `WiFi.disconnect(true, false)` (radio off, stored credentials kept), waits for the teardown (RSSI back to 0, bounded by `WIFI_SETTLE_MS`), then re-begins the same AP with the same channel/BSSID. That matters because `WiFi.disconnect(false, false)` leaves the radio up, so the STA netif can stay up, esp_netif never stops the DHCP client, and the retry inherits the accumulated DISCOVER backoff (next retransmit 16-32 s away) instead of starting a fresh ladder. Both connect paths call the one helper so they cannot drift. The association line (`Associated after Nms (RSSI x) - waiting for the address`) splits association from lease, and the retry logs `No address after Nms - restarting the network and trying <ssid> once more`. The deferred boot-line message cannot land on the AP screen — it fires at 2 s, long before any of this.
  - **SoftAP fallback, and its reboot timer.** `raiseSoftAP()` brings up the AP, the DNS captive portal and Improv; the optional auto-reboot is `if (SOFTAP_REBOOT_DELAY > 0) rtimer.once((uint32_t)SOFTAP_REBOOT_DELAY * 60, rebootTime);`, in minutes. `SOFTAP_REBOOT_DELAY` (options.h, default **0** = never) is the only control: the WebUI field, the stored `softapdelay` key (purged by `deleteOldKeys()`), the `softap` command and the `"softr"` field of the netserver GETSYSTEM payload were all removed, so re-enabling the reboot means editing options.h/myoptions.h.
  - **Build/size.** `sh1106_vs1053_3buttons` (8 MB partition) builds clean at **1,832,333** bytes flash and **72,236** bytes RAM with `BOOTLOG_TIME` off — 308 bytes *smaller* than the version before the retry rework, because the shared `wifiRestartForRetry()` helper replaced two inlined copies of the restart code. A build that dies at the `.bin` step (e.g. the uploader holding `firmware.bin`) leaves a stale sconsign, so the next build can link stale objects and report a plausible-looking size; delete `.pio\build\<env>\src\*.o` and rebuild rather than trusting it.
  - `retryStreamConnection` task (40 fast attempts, then a slow infinite tail) is cancelled through `MyNetwork::cancelStreamRetry()`, the single owner of `streamRetryTaskHandle` (called by commandhandler on playback-changing commands, by `player.prev()`/`next()`/`toggle()`, and by `utility.turnoff()`); the task also cleans itself up when conditions change (user stops, WiFi drops, or playback resumes)
- Coupling:
  - pushes display updates (`display.putRequest(...)`)
  - calls player/netserver hooks
  - reads/writes `config.store`
- Successful connect handling now stays internal to `network.cpp`; there is no remaining app-level weak `network_on_connect` callback.

## Network Recovery (the stall, the reset ladder, and the boot-stable marker)

Everything follows from one measured fact: **`Audio::connecttohost()` runs on the main task**, so a failing
stream connect blocks `loop()` for its whole timeout and the WebUI looks dead meanwhile.

**Two routines, not one.** `wifiReconnectionTask()` owns the *link* (`WIFI_STA_DISCONNECTED`, gated by
`network.beginReconnect`, cadence 5/10/30 then 60 s capped); `retryStreamConnection()` owns the *stream*
(stream died with the link up, gated by `network.lostPlaying`, cadence 1/3/6/12/15 s for 40 attempts then
60 s forever). Neither gives up, and a wedged stack raises no disconnect event - so the Wi-Fi routine never
runs, the stream task can only replay the same failing connect, and that is why the forced reset exists.

**The traps around that reset.** It is `WiFi.disconnect(true, false)` - never `eraseap = true` - and its budget
must be a **file-static**, because `WiFiReconnected` recreates the task and a local gives each instance a fresh
budget (four back-to-back teardowns, measured). The handle must be cleared before `vTaskDelete`, and
`WiFiLostConnection` must use `network.lostPlaying || player.isRunning()`, never an assignment: during a reset
nothing is playing, so an assignment wrote `false` and the Wi-Fi returned to a silent radio.

**Refusal vs wedge is decided by duration.** At or past the connect bound means a stale path; far below it means
the peer refused and the link has just proved itself. Only the ambiguous case probes, and only
`NET_STACK_WEDGED` resets: no link means the Wi-Fi routine owns it, a gateway TCP answer means the stack is fine
and the host refused, and both probes silent while the driver reports connected means **WEDGED - reset at the
first failure**. DNS is not part of it - it goes through the same stack, so it is the same evidence.

**Manual Wi-Fi and the boot marker.** `captureCurrentAp()` runs at GOT_IP *and* at the end of
`MyNetwork::begin()` (the handlers register after the boot connect), letting `wifiBeginFast()` reconnect by
BSSID/channel with no scan and giving up after `WIFI_FAST_ATTEMPTS`, since a moved AP needs the scan and a reset
does not. `WIFI_SETTLE_MS` applies only when `WiFi.mode()` reports a change, and an empty scan is retried once.
`Startup::deferBootStable()` re-stamps the countdown while unproven, and no-ops once settled.

**Instruments, all unconditional.** `MAIN_LOOP_STALL_MS` names the worst stage of a slow `loop()` (this is what
identified `player+saves`), `NVS_SLOW_WRITE_MS` logs a slow NVS commit, `[Heap]` gives internal free **and the
largest contiguous block** (what a TLS handshake needs), recovery logs under `Network`, and `Telnet::loop()`
must never block. Every cadence and threshold above is a macro in `options.h`, **except the connect bound**:
`m_connectTimeout_ms` / `_ssl` in `src/libraries/I2S_Audio/Audio.h` (1200 ms), separate from
`CONNECT_HTTP_HTTPS_TIMEOUT`, which doubles as the socket READ timeouts and must stay patient for slow streams.

**The reload storm is the trigger, and the guard was withdrawn.** A reload opens a new websocket before the old
closes (22 clients, 3 KB heap); the `MIN_MALLOC` guard in `onWsEvent()` went away because that websocket carries
every setting to `script.js` (`setupElement`), so a refused client shows the page its defaults and the user
thinks the radio forgot everything. The loader-side wait (`plans/network-recovery.md` §F11) was not built.

## `src/core/player.h` / `player.cpp`
- `player.h` declares player command queue, playback API, and status.
- Audio engine integration and playback sequencing.
- Main responsibilities:
  - initialize codec/I2S/VS1053
  - queue command handling (`PR_PLAY`, `PR_STOP`, `PR_VOL`, etc.)
  - station play/stop/toggle/next/prev flow
  - exact-match-first URL playback routing for `playurl` / preview resume (`queueResolvedUrl`, `resumeLastWebSource`)
  - volume conversion (`volToI2S`) including ES8311 path
  - SD/web mode specific playback behavior
  - error reporting and display/net updates
  - command queue depth: `xQueueCreate(10, ...)` — increased from 5 to prevent queue overflow during rapid mode-switch sequences (SD→web transitions) where multiple commands (PR_STOP, PR_PLAY, PR_VUTONUS) arrive before the first finishes processing.
  - direct playback lifecycle side effects for `rgbled` and `backlightControls` (start/stop + initial stopped-state sync)
  - `mute()`: volume-0 toggle backed by the private `_muteVol` member; uses raw `getVolume()`/`setVolume()` so `config.store.volume` and the displayed volume are deliberately untouched. Shared by the physical mute buttons, the `mute` command, and the IR mute button — the `DSP_DUMMY` suppression lives only at the physical-button call site.
  - `prev()` / `next()` / `toggle()` delegate retry cancellation to `network.cancelStreamRetry()` (previously duplicated inline).
- VS1053 SPI: `Player::Player()` constructor passes `&VS1053_SPIBUS` to the `Audio(CS, DCS, DREQ, SPIClass*)` constructor. `VS1053_SPIBUS` is the `SPIB` or `SPIA` object resolved by `options.h`. No `SPIClass` declared in `player.cpp` or `player.h`.
- Coupling:
  - updates display queue and websocket state
  - uses `config` station and mode state
  - interacts with radio-browser click reporting (clicks are gated on `!network.lostPlaying` so automatic retry reconnections do not fire clicks; only explicit user-initiated plays do)
  - calls `rgbled` and `backlightControls` directly during playback start/stop

## `src/core/audiohandlers.h` / `audiohandlers.cpp`
- Callback bridge used by the audio libraries.
- `audiohandlers.h` now declares the `AudioHandlers` module and the required free `audio_*` callback symbols; `audiohandlers.cpp` owns the implementation as a normal core translation unit.
- Converts decoder callbacks into:
  - metadata updates
  - title/station updates
  - bitrate/codec updates
  - error updates
  - SD EOF behavior
- Important for title/bitrate side effects to WebUI and display.
- Owns runtime artwork state and policy:
  - parses `StreamUrl='...'` from `audio_info(...)`
  - accepts `audio_icylogo(...)` as a fallback image source
  - exposes the filtered `image_url` to MQTT through `audioHandlers` getters
- Uses shared utility helpers for string normalization instead of keeping those helpers in `config.cpp`.
- Audio info/bitrate/ID3 notifications are emitted through shared logging macros so telnet+serial output stays consistent with the rest of the firmware log contract.

## `src/core/display.h` / `display.cpp`
- `display.h` declares Display class and display mode/change API.
- Render queue + display task + widget/page orchestration.
- **Theme/layout/invert runtime switching** — major refactor:
  - `_applyState()` — central state function: loads layout from PROGMEM, loads theme from PROGMEM (TFT only), applies invert (color swap + `metaBGConf_ptr` switch), reinitializes widgets, redraws. Called by all three commands (`theme`, `layout`, `inverttitle`).
  - `_setLayoutPointers()` — extracted 30-pointer setup shared by init and runtime paths.
  - `applyLayout()`, `applyTheme()`, `applyInvertTitle()` — thin wrappers that set config then call `_applyState()`.
  - `inverttitle` is runtime boolean (`config.store.inverttitle`). On TFT: swaps meta↔metabg colors + switches to `metaBGConfInv` layout. On OLED: color swap only. Uses conditional rendering at widget init time — no theme mutation.
  - `getThemeListJson()` / `getLayoutListJson()` — serve dropdown data to WebUI from PROGMEM `_themeNames[]` / `_layoutNames[]`.
  - `#ifndef HIDE_*` compile guards removed from `_reinitWidgets()` and `_buildPager()` — all widget init now uses runtime null-checks.
  - `_buildPager()` no longer calls `_reinitWidgets()` — state application is handled by `_applyState()`.
  - `_start()` calls `_buildPager()` then `_applyState()` — widgets created then initialized with correct state.
  - `DSP_INVERT_TITLE` compile-time macro eliminated.
- Buffer bar terminology was aligned to actual behavior:
  - `_heapbar` -> `_bufferbar`
  - `heapbarConf` -> `bufferbarConf`
- Input-buffer bar values still come from `player.inBufferFilled()`; only visual normalization changed via `BUFFERBAR_VISUAL_FULL_PERCENT`.
- Battery widget support is runtime-guarded: `_battery` null-check at every call site. Display configs that don't support battery leave `batteryConf` zeroed (height=0) — no compile-time guard needed.
- Main responsibilities:
  - initialize rendering task and widgets
  - mode switching (`PLAYER`, `VOL`, `STATIONS`, `LOST`, `UPDATING`, screensaver)
  - draw station/title/weather/clock/VU/bitrate/playlist
  - update progress bar during update flow
  - battery indicator rendering
- Coupling:
  - depends on `config.store` for many visual toggles
  - reads `network` time/weather, `player` status
  - title changes now directly trigger `rgbled.trackChange()` and `backlightControls.restart()` instead of a weak hook

## `src/core/netserver.h`
- Declares request enums, websocket/server globals, `StaticFileCache` class, `CachedFile` struct, and NetServer API.
- `StaticFileCache` — PSRAM-backed cache for static WebUI files (files from `Config::wwwFiles[]`).
- `CachedFile` — per-file entry: URL path, plain data pointer, gzipped data pointer, content-type.
- Contains embedded fallback HTML templates (`emptyfs_html`, `index_html`, `emergency_form`).

## `src/core/netserver.cpp`
- HTTP + WebSocket + upload/update + search/curated orchestration.
- Main responsibilities:
  - static file serving from PSRAM cache (via `StaticFileCache`) — no LittleFS reads during HTTP serving
  - fallback to LittleFS for dynamic files not in cache (`searchresults.json`, `curated.json`, etc.)
  - `handleNotFound`: checks PSRAM cache first for GET requests, cache-busting `?v=` stripping removed (max-age=60 handles freshness)
  - `handleIndex`: serves `player.html` from PSRAM cache for `GET /`
  - route handlers (`/`, `/search`, `/update`, `/locale.json`, `/ready`, etc.)
  - `/visuals.json` — the VU style list, built **at request time** from one static `{ id, label, needsPcm }` table, so the WebUI select can only offer what the running build can draw: waveform and Lissajous are omitted on a VS1053 because they need PCM, and the labels for every other style are identical on both backends. The ids are `vuStyle_e`, the same numbers that travel in `vustyle=<n>` and come back in `GETSCREEN`. There is deliberately **no box-based filtering**
  - `fileCache.loadAll()` called in `begin()` before `webserver.begin()`
  - `invalidateCache()` public method exposed for `utility.cpp` runtime updates
  - `Cache-Control: max-age=60` for all cached static files (was 3600)
  - `/settings.html`, `/update.html`, `/ir.html` no longer served via `index_html[]` — handled by PSRAM cache fallthrough
  - websocket command parsing and outbound updates
  - state request queue processing (`GETSYSTEM`, `GETSCREEN`, `GETLOCALE`, etc.)
  - IR websocket helpers: `irToWs()` (protocol + code) and `irValsToWs()` (the active button's 3 codes, read through `config.irCodes()`)
  - online update check/start tasks
  - radio-browser search and curated task management
  - exact-match-first preview/add handling on `/search`; unmatched preview now uses the same direct URL playback path as `playurl` instead of a mutating playlist scan
  - centralized logging for search/curated/playback/radio-browser-click/update/not-found paths via `FUNCTIONLOG`
- Coupling:
  - uses `cmd.exec(...)` from commandhandler
  - emits JSON consumed by `data/www/script.js`
  - `GETSCREEN` now also carries `invtitle`, `layout`, `theme` **and `vustyle`** fields for WebUI dropdown state
  - Virtual endpoints `/themes.json` and `/layouts.json` serve dropdown data from PROGMEM arrays; `/visuals.json` is the first *runtime* one (built per request, not from PROGMEM), and the WebUI loads it through `loadVisuals()` + `populateNamedDropdown('vustyle', data)`
- Readiness detail:
  - `/ready` returns `{"ready":true}` only when `netserver.bootReady` is true, required web files exist, and network state is stable (`CONNECTED` + `WL_CONNECTED`, or `SDREADY`).
- OTA note:
  - OTA start/end/error callbacks use `FUNCTIONLOG`.
  - OTA progress now uses `FUNCTIONLOG` (line-oriented output, no raw `\r` streaming path).

## `src/core/commandhandler.h` / `commandhandler.cpp`
- `commandhandler.h` declares command execution API for command strings.
- Central command router for WS, URL params, MQTT, and telnet fallback paths.
- Main responsibilities:
  - map `key=value` commands into config/player/display/network actions
  - request websocket state snapshots
  - persist settings with `config.saveValue(...)`
  - source-aware command policy (`WebSocket`, `HttpUrl`, `Mqtt`, `Telnet`) and shared non-WebUI blocklist checks for HTTP/MQTT/Telnet ingress
  - own shared command aliases across ingress channels (`playstation`/`play`, `boot`/`reboot`, `vol+`/`volup`, `dim`/`brightness`, `dspon`/`screenon`)
  - player-command parity helpers (including exact-match-first direct URL playback command routing for `playurl` / `burl`)
  - trigger curated operations and locale update tasks
  - cancel the stream retry task (`network.cancelStreamRetry()`) before executing user-initiated playback commands (`stop`, `playstation`, `prev`, `next`, `toggle`, `turnoff`, `burl`, `mode`, `submitplaylist`) so explicit user actions always interrupt automatic reconnection loops
  - `turnon` / `turnoff` delegate to `utility.turnon()` / `utility.turnoff()`; the `mute` command maps to `player.mute()`
  - IR recorder commands: `irbtn` resolves a button **name** via `config.irButtonByName()` (`-1` stops recording and saves), `chkid` selects the slot, and `irclr` clears a slot through `config.clearIR()`
- Critical coupling file for setting changes.
- New commands: `theme` (theme switching), `layout` (layout switching), `inverttitle` (invert title toggle). All persist via `saveValue` and trigger `display._applyState()`.

## `src/displays/themes.h`
- **NEW FILE** — Runtime theme switching data.
- `ThemeData` struct: 33 `uint16_t` color fields + `playlist[5]` — mirrors `config.h` `theme_t`.
- `const ThemeData _themes[] PROGMEM` — array of theme presets (default + imported).
- `const char _themeNames[][32] PROGMEM` — display names for WebUI dropdown.
- `RGB(r,g,b)` macro packs 8-bit RGB into RGB565 `uint16_t`.
- Populated by `importtheme.py` script.

## `src/displays/importtheme.py`
- **NEW SCRIPT** — imports old-style `#define COLOR_*` theme files into `themes.h`.
- Handles `#ifdef`/`#ifndef` branching to generate multiple theme variants (permutation).
- Smart fallbacks for missing colors (e.g., `.dow` ← `.date`, `.battery` ← `.rssi`).
- Meta fallback for everything else (uses `.meta` color instead of zeros).
- Name truncation at 31 chars for `_themeNames[][32]`.
- `--dry-run` writes to `.new.h`.

## `src/core/controls.h` / `controls.cpp`
- `controls.h` declares `class Controls` with public interface: `init()`, `loop()`, `setEncAcceleration()`, `setIRTolerance()`, `flipTS()`, `controlsEvent()`.
- `extern Controls controls;` provides the global instance; callers use `controls.init()`, `controls.loop()`, etc.
- All internal helpers (`onBtnClick`, `encodersLoop`, `irLoop`, etc.) are private class methods.
- Static trampoline methods (`_btnClickCb`, etc.) used for `OneButton` callbacks (function-pointer API; cannot capture `this`).
- `readEncoderISR` / `readEncoder2ISR` remain free functions with `IRAM_ATTR` (ISR constraint; access file-scope `encoder`/`encoder2` directly).
- Physical controls integration:
  - OneButton
  - rotary encoders
  - touchscreen gestures
  - IR remote decoding
- Converts hardware input events into same core actions used by WebUI (`controlsEvent`, player commands, display mode changes).
- `Controls::loop()` now calls `backlightControls.controlsLoop()` directly for non-PLAYER backlight wake behavior.
- IR record debug text now routes through centralized logging macros.
- IR dispatch is name-based: `irLoop()` iterates `config.irButtonCount()`, matches codes from `config.irCodes(button)`, then switches on the behaviour id from `config.irAction(button)` (`IRACT_POWER`, `IRACT_MUTE`, `IRACT_UP`, `IRACT_DOWN`, `IRACT_PREV`, `IRACT_NEXT`, `IRACT_PLAY`, `IRACT_MODE`, `IRACT_HASH`, `IRACT_DIGIT`). Digit buttons derive their value from the `n0`…`n9` key. The old positional `IR_UP`…`IR_HASH` enum is gone. Power/mute/mode are local actions and are allowed while offline or showing `LOST`.
- Physical mute (`EVT_ENC2_SW` / `EVT_BTN_MODE` double-click) calls `player.mute()` and keeps the `DSP_MODEL == DSP_DUMMY` no-op guard at the call site.
- Learning a new IR code calls `config.clearDuplicateIR()`, which zeroes that same code in every other button and slot so only the newest copy survives (a code can no longer be shadowed by an earlier duplicate at match time). Empty slots hold `0` and are never matched. The clear happens in RAM; it is persisted by the existing `saveIR()` on recording stop, and `irValsToWs()` is re-sent only when something was actually cleared.
- Screensaver wake hardening:
  - `controlsEvent()` now flushes pending display requests (`display.resetQueue()`) and zeroes screensaver tick counters before queueing `NEWMODE, PLAYER` when waking from `SCREENSAVER`/`SCREENBLANK`, preventing one-detent rotary wake races where a stale queued screensaver mode request could immediately re-apply.

## `src/core/telnet.h` / `telnet.cpp`
- Telnet and serial command handling.
- Responsibilities:
  - manage client sessions
  - read input lines with CR/LF-pair handling so Enter submits immediately across CR/LF client variants and empty Enter events are preserved
  - apply explicit 2000 ms stream timeout configuration for serial and per-client telnet streams
  - normalize command strings (`key=value`, `key value`, `key(value)`) plus minimal payload-shape handling (`play` value-shape handling)
  - route commands through `cmd.exec(...)` with source `Telnet`
  - keep command handling output-minimal (no telnet-specific reporting command table)
- Important:
  - acts as secondary control channel parallel to WebUI
  - command handling is intentionally kept near-parity with MQTT/HTTP routes
  - `Telnet::printf(...)` is transport-only and no longer echoes to serial
  - `help`, `quit`, and `bye` are handled locally in telnet before commandhandler dispatch
  - `quit` / `bye` silently disconnect only the issuing Telnet client
  - empty input lines now re-show prompt (`> `), aligning interactive UX with common telnet clients

## `src/core/mqtt.h` / `mqtt.cpp`
- MQTT integration if `MQTT_ENABLE` compile flag exists.
- `mqtt.h` declares `class Mqtt` with `init()`, `loop()`, `publishStatus()`, `publishVolume()`, `publishPlaylist()`.
- `extern Mqtt mqtt;` (inside `#ifdef MQTT_ENABLE`) provides the global instance.
- Private static callback methods (`_connectCb`, `_onConnect`, `_onDisconnect`, `_onMessage`) used for AsyncMqttClient API (static required by library callback interface).
- Responsibilities:
  - connection lifecycle
  - subscribe to `.../command`
  - publish status/playlist/volume
  - status payload now includes `image_url` (HTTP/S image-only artwork URL used by Home Assistant)
  - parse command payload forms (`key=value`, `key value`, `key(value)`, raw URL)
  - apply minimal payload-shape normalization (`play` value-shape handling) then dispatch through `cmd.exec(...)` with source `Mqtt`
  - apply explicit non-WebUI blocklist rejections for unsupported MQTT-origin commands
- Coupling:
  - command behavior is now primarily centralized in commandhandler.
  - `ARTWORK` queue events in `netserver` trigger MQTT status republishes even without a WebSocket payload.
  - artwork payload data is read from `audioHandlers`, not from `config.station`.
- Status buffer sizing now derives from `STATION_FIELD_LENGTH` plus `MQTT_URL_SIZE`, replacing the older duplicated browse-URL size macro.

## `src/core/utility.h` / `utility.cpp`
- Shared helper module following the standard core `class + global instance` pattern (`Utility utility;`).
- Current responsibilities:
  - `stripWhitespace(char*)`
  - `stripWrappingQuotes(char*)`
  - `ipToStr(...)`
  - `escapeQuotes(...)`
  - playlist CSV parsing and station lookup/load helpers
  - WiFi credential parse/save/import helpers
  - deep-sleep entrypoints (`doSleepW`, `sleepForAfter`)
  - standby on/off helpers `standbyon()`, `standbyoff()`, `standbytoggle()` (shared by the `standbyon`/`turnon` and `standbyoff`/`turnoff` commands and the IR power button); `standbyoff()` also calls `network.cancelStreamRetry()`
  - LittleFS file-maintenance helpers shared with startup and WebUI update paths:
    - `pruneLittleFS()`
    - `deleteMainwwwFile()`
    - `updateFile(...)`
- Holds small reusable scratch/state buffers (`ipBuf`, `stationBuf`) plus the sleep duration state and sleep `Ticker`; it still does not own playback/artwork runtime state.
- Current consumers include `audiohandlers.cpp`, `battery.cpp`, `commandhandler.cpp`, `config.cpp`, `display.cpp`, `netserver.cpp`, `network.cpp`, `player.cpp`, and startup/update flows.

## `src/core/battery.h` / `battery.cpp`
- `battery.h` declares `class Battery` (real class under hardware guard; no-op stub in `#else`); `extern Battery battery;` provides the global instance.
- Public interface: `init()`, `bootStatus()`, `isInitialized()`, `getStatus()`, `formatStatusLine()`, `loop()`, `applyPowerPolicy()`, `recalcNow()`, `calibrate()`.
- All ADC/inference state and helpers are private members/methods.
- Battery monitoring/calibration/inference implementation.
- Responsibilities:
  - ADC sampling and filtering
  - battery presence detection
  - charge/discharge inference with candidate windows
  - threshold state (`low`, `critical`) tracking
  - battery-driven brightness reduction / recovery and critical deep-sleep policy
  - status formatting for telnet/WebUI
  - triggers display and websocket updates
- Logging note:
  - battery status/debug/inference messages now use centralized logging macros (including `BATTERY_DEBUG` paths), replacing direct serial/telnet prints.

## `src/core/backlightcontrols.h` / `backlightcontrols.cpp`
- `backlightcontrols.h` declares `class BacklightControls` (real class under `BRIGHTNESS_PIN` + `DSP_DIMMING_ENABLED` guards; no-op stub in `#else`); `extern BacklightControls backlightControls;` provides the global instance.
- Public interface: `init()`, `restart()`, `controlsLoop()`.
- Responsibilities:
  - own persisted idle-dimming timer state and non-blocking brightness ramp
  - use `config.store.dimmingEnabled`, `dimmingTimeout`, and `dimmingBrightness` instead of board-only compile-time dimming thresholds
  - clamp the dim target to the current screen brightness, restore configured brightness, and restart the idle timer on explicit activity/settings events
  - centralize the former `main.cpp` backlight code without moving it into the display task owner
- Explicit call sites:
  - `main.cpp` after `config.init()`
  - `config.cpp` screen-default reset path
  - `commandhandler.cpp` brightness / dimming / display-on commands
  - `player.cpp` playback start/stop paths
  - `display.cpp` title-change path
  - `controls.cpp` loop path
  - `battery.cpp` low-battery recovery / restore path when the dimmer feature is enabled

## `src/core/rgbled.h` / `rgbled.cpp`
- `rgbled.h` declares `class RgbLed` (real class under `RGB_LED_PIN` guard; no-op stub in `#else`); `extern RgbLed rgbled;` provides the global instance.
- Public interface: `init()`, `isInitialized()`, `set()`, `playing()`, `stopped()`, `trackChange()`, `loop()`.
- Optional RGB LED state machine:
  - playing/stopped colors
  - track-change flashing
  - optional cycle behavior

## `src/core/sdmanager.h` / `sdmanager.cpp`
- `sdmanager.h` declares SD manager API and FS integration wrapper.
- SD lifecycle and SD playlist indexing.
- Responsibilities:
  - mount/retry/unmount — `start()` attempts up to 4 `SD.begin(SD_CS, ...)` calls, early-returning on success (delays only between retries, not after success)
  - card-present checks
  - recursive scan and media file playlist/index creation
  - scan/index progress and errors now use centralized logging macros (`SERIALLOGDOT`, `ERRORLOG`)
- SPI bus: `SDREALSPI` macro resolved at compile time — `SPIB` when `SD_SPI == 'B'` and `SPIB_SCK` defined, otherwise `SPIA`. Both buses are initialized in `Config::init()` before `SDManager::start()` runs. No `SPIClass` declared in `sdmanager.cpp`.
- SD CS pin is `SD_CS`. Guard macro: `#if SD_CS!=255`.
- Coupling:
  - consumed by config/player for SD mode.

## `src/core/touchscreen.h` / `touchscreen.cpp`
- Touch controllers:
  - XPT2046
  - GT911
  - FT6336
- Responsibilities:
  - init and orientation/flip
  - swipe and tap/long press mapping to control events
  - touch debug coordinates now route through centralized logging macros

## `src/core/rtcsupport.h` / `rtcsupport.cpp`
- RTC init/get/set wrappers for DS3231/DS1307 when configured; `rtcsupport.h` has compile guards.

---

## WebUI Per-File Map (`data/www`)

## `data/www/script.js`
- Main runtime script.
- Responsibilities:
  - websocket connect/reconnect
  - parse inbound JSON payloads
  - dynamic page loading (`player`, `settings`, `update`, `ir`)
  - shared page bootstrap helpers for logo/version/i18n application
  - safe lazy fallback for script2-exposed functions (`ensureFunctionLoaded`)
  - control dispatch from DOM (`data-command`)
  - playlist editor/import/export logic and curated integration
  - online update UI progress handling
  - shared ready-aware redirect helper (`redirectWhenReady`) used by update and reboot flows
- Reboot/update redirect nuance:
  - `redirectWhenReady(...)` only redirects after it has observed at least one not-ready state, preventing a false-positive redirect against the still-running pre-reboot instance.
  - `/ready` probes now use a short client-side fetch timeout so reboot/reset flows do not stall waiting on a dead device connection during restart.
  - reboot and update redirect calls now explicitly apply a 1-second post-ready grace in JavaScript before navigation.
  - manual upload completion now uses a 60 second fallback, while OTA still uses 180 seconds; both can redirect early as soon as `/ready` reports true.
  - mDNS rename (`restartmdns`) calls `MDNS.end()` + `MDNS.begin()` at runtime via `NetServer::restartMdns()` — no reboot. Browser-side: sends `mdnsname=`, swaps the button row for a status message, then polls the new `.local` host with `redirectWhenReady` (8s timeout, 500ms post-ready grace). If mdnsValue is empty, saves silently without redirect.
- consolidated with `data/www/dragpl.js`
  - Playlist drag-and-drop reorder behavior.

## `data/www/options.js`
- Settings page behavior.
- Responsibilities:
  - timezone JSON loading and dropdown population
  - locale list loading and locale switch logic
  - weather provider field visibility logic
  - **theme/layout dropdown loading** — fetches `/themes.json` and `/layouts.json`, populates `#themeId`/`#layoutId` dropdowns, sends `websocket.send("theme=N")` / `websocket.send("layout=N")` on change
  - `afterSetupElement` hook restores current `themeId`/`layoutId` from WebSocket data
  - apply handlers for locale/weather/mqtt/wifi
  - reboot/reset/format status screen with per-action behavior:
    - reboot/reset use ready-aware return-to-root with shorter fallback (15s)
    - format LittleFS shows reboot status but skips automatic reload

## `data/www/player.html`
- Player page structure (playlist, controls, sliders, status elements).

## `data/www/settings.html`
- Settings page structure with grouped sections and `data-command` bindings.
- Contains element IDs expected by websocket payload mapping.

## `data/www/update.html`
- Update page layout (manual upload + online update controls).

## `data/www/ir.html`
- IR recording and assignment UI.
- Every `.irbutton` carries a `data-irid` name (`power`, `mute`, `up`, `down`, `prev`, `next`, `play`, `mode`, `number`, `n0`…`n9`) that maps 1:1 to the `irstore` field / NVS key, so DOM order is irrelevant.
- The shell loads the page body from `irrecord.html`; the `/ir.html` route itself is handled by the PSRAM cache fallthrough.

## `data/www/search.html`
- Search UI for radio-browser integration.

## `data/www/curated.html`
- Curated list browsing/import page.

## `data/www/script2.js`
- Consolidated helper script loaded by main shell and standalone search/curated pages.
- Contains logic previously in `ir.js`, `updform.js`, `playstation.js`
  - station preview/play helper (`sendStationAction`)
  - online update check/start UI helpers
  - IR setup/learn interactions (`initControls`, `checkSelect`, `irClear`, `backRecord`); `irbuttonClick()` sends the button's `data-irid` name as `irbtn=<name>` and `irbtn=-1` on deselect
- also consolidated with `data/www/locale.js`
  - i18n runtime helper (`t(...)`) and translation application (`applyI18n`).
  - Applies key-based translations to DOM and fallback behavior.

## `data/www/search.js`
- Search page API calls, pagination, result actions, and import hooks.

## `data/www/curated.js`
- Curated list fetch/load/import page logic.

## `data/www/style.css`
- Primary stylesheet.

## `data/www/theme.css`
- Theme override variables/colors.

## `data/www/locales.json`
- Locale-code to display-name mapping for selector.

## `data/www/timezones.json`
- Timezone label -> POSIX tz mapping used by settings UI.

## `data/www/rb_srvrs.json`
- Radio-browser server source list used by search task fallback and randomization.

---

## Displays Folder Map (`src/displays`) - Grouped

## Core display abstractions
- `src/displays/dspcore.h`
  - common display core wrapper and API layer used by `core/display.cpp`.
- `src/displays/widgets/widgets.h`, `widgets.cpp`, `widgetsconfig.h`
  - widget classes (scroll, text, bars, VU, clock, playlist, etc.).
  - **Boot-line messages** (what the `_bootstring` line says while the boot is blocked): one request type per message in `displayRequestType_e` (`common.h`) plus a case in `Display::draw()` that does `_bootstring->setText(l10n(L10N_MSG_...))`. The three are `WAITFORSD`, `FORMATTING` and `SCANNINGWIFI` — the last for the boot scan, which blocks the radio for ~5.7 s with nothing else to show, and it is sent from `wifiBegin()` just before the scan. **Senders must include a short `delay()` after `display.putRequest(...)`**: it only queues, and the blocking work that follows can otherwise start before the display task has drawn the line (the `FORMATTING` sender does the same). The strings live in the generated `src/locale/dsplocale.h`, so a new message needs its key added to all 36 `src/locale/display/*.json` files and that header regenerated; measured cost of one new key across the 36 locales is **~5 KB of flash**, which is the number to weigh before adding one.
  - `TextWidget::setText()` is **virtual** (all three overloads), with `ScrollWidget` and `NumWidget` marking their matching overloads `override` so the compiler enforces the match. This matters because a subclass can legitimately be held in a `TextWidget*`: the boot line is exactly that case (`Display::_bootstring` is a `TextWidget*` over a `ScrollWidget`). Bound non-virtually, a call through that pointer runs the **base** version, which never sets `_doscroll`/`_x` and measures with the base `_charWidth` — the boot line then could not scroll and, for a string wider than the window, was painted at an underflowed centre offset, which reads as "nothing drawn". `_draw()` was already virtual, so this removes a static/dynamic mismatch rather than adding a mechanism.
  - `TextWidget::_realLeft()` **clamps** rather than underflowing: when `_textwidth` is not smaller than the available width it returns `0` (the widget's own left edge) instead of wrapping the subtraction to ~65500 and painting off-panel. Only reachable for text wider than its space — a `ScrollWidget` scrolls instead and a plain `TextWidget` is conf-sized — so the clamp is insurance against the invisible-rather-than-misplaced failure mode.
  - The boot text line (`Display::_bootstring`, built in `Display::_bootScreen()`) is a `ScrollWidget` fed from the conf's plain `bootstrConf` `WidgetConfig`, so a string that fits is drawn statically at the conf's align (WA_CENTER still rules) and a longer one parks at the edge and scrolls. Its window and buffer are derived in code (`width = MAX_WIDTH`, `buffsize = BOOTSTR_LEN` 128, uppercase), and its **scroll cadence is borrowed from the panel's own `apSettConf`** — `startscrolldelay`, `scrolldelta` and `scrolltime` only, never that entry's `left/top/width/buffsize/fontsize`, which belong to its own line and differ on the round TFT (`left = TFT_FRAMEWDT+32`, `width = MAX_WIDTH-64`) and the 220x176 (`width = DSP_WIDTH+10`). That inherits each panel's tuned step: 1px on the OLEDs and small TFTs, 2px on the 220x176 and every panel 240px and up, 4px on the 428x142, and 5/6px on the mono LCDs where a step is a whole character because their refresh cannot take per-pixel repaints. `apSettConf.widget.textsize > 0` is the guard, because `displayLCD16x2conf.h` and `displayLCD20x4conf.h` leave the entry as `{ }`, which zero-initialises it and would leave the boot line standing still — the fallback is 0 / 1 / `SCROLLTIME`.
  - `ProgressWidget` is the boot screen's animated dots line, with exactly one instantiation (`Display::_bootScreen()`), so its behaviour is the boot screen's. The line is `speaker + runway + boot glyph`, hard against both glyphs, and `ProgressConfig` is `{ frame interval ms, line character width, blob elements }` — every conf carries that header above the field, and `importlayout.py` emits it for generated confs. `width` is the **whole line budget in characters**: `init()` derives the runway as `width - 2`, counting each glyph as **one character regardless of its byte length** (the SD pair renders one column wider than the rest, which is invisible on a single row of pixels). **There are two paint paths, and that split is what keeps a TFT flicker-free**: `_draw()` is the full paint — speaker, the runway with the blob where it belongs, boot glyph — reached only from activation, layout changes and screensaver restarts, so the two static glyphs are drawn once and then never touched; `_progress()` is the animation, and it paints **only the cells that differ from the previous frame**, at most two of them (the cell the blob left, the cell it entered), against a whole-line erase plus fourteen glyph writes in the single-path version. `_fieldX` records the field's x origin at the last full paint, and `_painted` guards the first tick so a delta can never be painted against a picture the widget did not put on the panel; both paths place column `c` at `_realLeft() + (frameChars + c) * _charWidth`, which is why a full paint cannot shift the dots. The reason is the panel rather than the animation: an SH1106 is a framebuffer whose refresh hides an erase, while an ILI9488 shows every write, so the same code flickers on one and not the other. The blob grows in at the speaker, slides right one column per frame and has its head eaten at the far end, which is what makes the dots read as vanishing into the boot glyph; the cycle is `runway + blob` frames (the 128x64 OLED conf's `{ 90, 14, 4 }` is a twelve-column runway and sixteen frames, 1.44 s), and the character count is identical on every frame so the centred position never moves. A blob at or above the runway never reaches the sliding phase — it just grows and snaps back. **The blob element is an icon codepoint, `\026` (VOL_75) from `icons.h`, not a font glyph**: that is what makes it line up, because icon codepoints all share the same seven-row grid as the speaker and the boot glyph, whereas a font bullet carries its own vertical metrics and sits off centre between them. **The text buffer is sized in BYTES rather than characters, for a reason worth keeping**: with the earlier two-byte U+00B7 dot, a character-count buffer truncated the line mid-dot, a dangling `0xC2` made the renderer take the terminator as its second byte and draw past the NUL (a glyph no font knows), and the changed character count moved the centred x every frame. `_buffsize` is `frame bytes + runway + one extra byte per blob element + 1`, which reduces to `frame bytes + runway + 1` while the element stays one byte. Nothing here uses `Widget::_width`, which `Widget::init()` zeroes and `moveTo()` rewrites — sizing from that drew a completely blank line. `_scrolldelay` is set in `init()`; it was previously never initialised.
  - `SliderWidget` buffer/volume bar rendering now repaints the full inner area each update to avoid stale pixels after page/mode transitions.
- `src/displays/widgets/pages.h`, `pages.cpp`
  - page and pager composition framework.

## Display driver files (`src/displays/display*.h/.cpp`)
- Similar pattern:
  - init hardware
  - draw primitives/text/pages
  - sleep/wake/flip/invert where supported
- `flip()` maps `config.store.flipscreen` to `setRotation(...)`. Rotation values follow the Adafruit GFX convention (0=0°, 1=90°, 2=180°, 3=270°) relative to each controller's native orientation. `ROTATE_90` (optional, for square panels) is applied only when `DSP_WIDTH==DSP_HEIGHT` and adds 90° to that controller's normal base rotation.
- Files include:
  - `displayST7735*`, `displayST7789*`, `displayST7796*`
  - `displayILI9341*`, `displayILI9488*`, `displayILI9225*`
  - `displaySSD1306*`, `displaySSD1305*`, `displaySH1106*`, `displaySSD1327*`, `displaySSD1322*`
  - `displayN5110*`, `displayGC9A01A*`, `displayGC9106*`, `displayST7920*`, `displayLC1602*`

## Display config files (`src/displays/conf/*.h`)
- Mostly widget coordinates/sizing/visibility for each panel class.
- Treated as layout maps rather than logic-heavy files.
- Consolidated canonical conf files now route multiple models by panel type/dimensions:
  - `displayOLED128x64conf.h` for SH1106/SH1107/SSD1305/SSD1306 128x64 OLED class.
  - `displayTFT480x320conf.h` for ILI9488/ST7796 class.
  - `displayTFT320x240conf.h` for ILI9341/ST7789 class.
- The canonical files are intentionally deduplicated masters without per-model `#if DSP_MODEL...` branches; model-specific legacy deltas were removed in favor of one shared layout baseline per family.
- Display driver headers now include conf files directly; custom fallback include blocks using `__has_include("conf/*_custom.h")` were removed.
- `metaBGConf` / `metaBGConfInv` — runtime-switchable station-name background (full bar vs thin line on TFT; bar vs no-bar on OLED). Controlled by `config.store.inverttitle`.
- OLED configs: `metaBGConf` may be `{ }` (normal mode no bar), `metaBGConfInv` has the bar (inverted mode). `importlayout.py` auto-swaps when importing into OLED targets.
- All conf files must define every `WidgetConfig` / `FillConfig` / `ScrollConfig` / `ProgressConfig` / `VUBandsConfig` / `MoveConfig` field that `display.cpp` references — nothing is optional at link time. Disabling a feature is done by zeroing the relevant struct (e.g. `height=0`, `buffsize=0`, `dimension=0`); `display.cpp` checks those sentinel values at runtime and skips the widget. This eliminates the old `HIDE_*` compile-time macro system entirely.

## Display tools
- `src/displays/tools/utf8To.*`
- `src/displays/tools/utf8_common.*`
- `src/displays/tools/utf8Latin.*`
- `src/displays/tools/utf8Cyrillic.*`
- `src/displays/tools/commongfx.h`
- `src/displays/tools/psframebuffer.h`
- `src/displays/tools/oledcolorfix.h` — OLED monochrome color initialization. `DSP_INVERT_TITLE` ifdef removed; `metafill` corrected to `TFT_FG` (was `TFT_BG` — invisible on black screen).

Purpose:
- text normalization/transliteration and glyph handling
- display utility support

## Display fonts/assets
- `src/displays/clockfonts/*` for digit/font assets.
- `font15.h` is wired for 128x64 mono-OLED clock paths (SH1106/SH1107, SSD1306 128x64, SSD1305). Clock font dispatch headers now live under `src/displays/clockfonts/` (`font15.h`, `font19.h`, `font35.h`, `font52.h`, `font70.h`), and DS_DIGI/Chunky6 families also live there (`src/displays/clockfonts/DS_DIGI/`, `src/displays/clockfonts/Chunky6/`). `font15.h` maps CHUNKY6 modes directly to Chunky6_15 variants; `font19.h` is retained for possible future use. For clock rendering fallback, widgets now force `CLOCKFONT5x7` when `TIME_SIZE<15`, or when `TIME_SIZE==15` with `CLOCKFONT` set to `YO_MONO`.

## GFXfont Rendering Pipeline (`src/displays/tools/commongfx.h`, `psframebuffer.h`)

`DspCore::_writeGlyph(uint16_t cp)` is the central glyph renderer replacing the legacy 256-slot glcdfont system. Key behaviors:

- **Dispatch:** When `gfxFont != NULL && gfxFont != &DisplayFont` (a special clock font is active), delegates to `Adafruit_GFX::write()` which uses the font's native `drawChar`. Otherwise renders via `&DisplayFont` (the configured Unicode GFXfont).
- **Icon rendering:** Codepoints `0x01-0x1F` map to `ICON_TABLE[]`; rendered with `startWrite()`/`endWrite()` wrapping (needed for Adafruit SPI TFT drivers).
- **Font glyph rendering:** Similarly wrapped in `startWrite()`/`endWrite()`. The inner loop iterates all `width` bitmap columns but only renders columns where `(xOffset + xx) < xAdvance` — prevents glyph bleed beyond the cell boundary (fixes colon/digit overlap on SH1106 YO_MONO). Bleed columns have their bits consumed from the bitmap stream but are not rendered.
- **Space handler:** Advances cursor by the font's first-glyph `xAdvance` without drawing pixels.
- **Unrenderable fallback:** Falls through `foldAccent()`; if still unmapped, advances cursor by the font's `xAdvance` (blank space).
- **`psframebuffer.h`** mirrors the same `_writeGlyph` logic for PSRAM-framebuffer TFT displays.

Important rendering invariants:
- Never call `startWrite()`/`endWrite()` in `write()` or `writePixel`/`writeFillRect` overrides — SPI nesting causes hangs on Adafruit TFT drivers.
- The `gfxFont == NULL` case (YO_MONO / display font) runs through `_writeGlyph` using DisplayFont, NOT the built-in glcdfont. This means glyph metrics (yAdvance, yOffset, xAdvance) come from DisplayFont.

## Display Widget Memory Ownership

The widget classes mix two allocation conventions; release must match allocation:

| Member | Owner | Allocated with | Released with |
|---|---|---|---|
| `_text`, `_oldtext` | `TextWidget` (and `NumWidget`, which duplicates it) | `malloc` | `free` |
| `_sep`, `_window` | `ScrollWidget` | `malloc` | `free` |
| `_fb` | `ScrollWidget`, `ClockWidget` | `new psFrameBuffer` | `delete` |
| `_canvas` | `VuWidget` | `new Canvas` | `delete` |
| `_caps` *(removed)* | `VuWidget` | was an in-object array for the spectrum's per-band caps | — the caps were cut on review, so the widget carries no per-band state |

Rules:
- Never `free()` a `new`-allocated object: `free()` skips the destructor and leaks the internal buffer.
- `init()` is re-called on every layout/theme change, so it must release its previous buffers first.
- Resize an existing `psFrameBuffer` with `freeBuffer()` then `begin()`, never by allocating a second one.

### Optional widget guard fields

Each config type has its own field that makes a widget meaningful, and that is what the creation guard in `display.cpp` must test — never a coordinate, since `{0,0}` is a legitimate position:

| Config type | Fields | Guard field |
|---|---|---|
| `WidgetConfig` | `left`, `top`, `textsize`, `align` | `textsize > 0` |
| `ScrollConfig` | `widget`, `buffsize`, `uppercase`, `width`, ... | `buffsize > 0` |
| `FillConfig` | `widget`, `width`, `height`, `outlined` | `height > 0` |
| `BitrateConfig` | `widget`, `dimension` | `dimension > 0` |

`_fullbitrate` and `_bitrate` are alternatives: an empty `.fullbitrateConf` falls back to `.bitrateConf`, and `_reinitWidgets` must tear down whichever one is no longer wanted even when the replacement config is itself empty.

## VU Widget Rendering (TFT vs OLED, and the runtime style)

`VuWidget` lives in **`src/displays/widgets/widget_vu.h` / `widget_vu.cpp`** — it moved out of `widgets.h`/`widgets.cpp` because it is now seven draw paths over one box. `widget_vu.h` includes `widgets.h`; **`widgets.h` must not include `widget_vu.h`** (that is the include cycle) — `display.cpp` includes it explicitly instead. It is compiled for every graphics display (`#if !defined(DSP_LCD)`); character LCDs get inert stubs in the same file.

`_draw()` is a **dispatcher**: it resolves the area once into `_len`/`_thk`/`_cw`/`_ch`, calls `_levels()`, fills the whole area with the background, then switches on `config.store.vustyle` and blits once at the end. The styles are `vuStyle_e` from `widgetsconfig.h` — **seven of them**: Bars (0), Digital LED (1), History (2), Spectrum Reflect (3), Waveform (4), Lissajous (5), Spectrum Mirror (6). The ids are persisted in `config.store.vustyle` and are the keys in `/visuals.json`, so they must never be renumbered again (Spectrum A was cut, and the ids shifted, before the first release) — Spectrum Mirror is id 6 rather than id 4 for exactly that reason, and `/visuals.json` lists it beside Spectrum Reflect so the two still sit together in the dropdown. `_fillLocal()` is the only helper that knows the pixel surface:

| | TFT (`DSP_TFT`) | OLED (`DSP_OLED`) |
|---|---|---|
| Buffer | `Canvas *_canvas`, 16-bit (`GFXcanvas16`) | none — the driver owns the framebuffer |
| `init()` | allocates the canvas | no allocation |
| Fills | `_canvas->fillRect` at widget-local coords | `dsp.fillRect` at `_config.left/top` + local coords |
| Transfer | one `startWrite`/`setAddrWindow`/`writePixels`/`endWrite` | none — `DspCore::loop()` calls `display()` |

The shared box fill in `_draw()` is the clear for **every** style, including the tail-clear bar family: every fill and `_clear()` stops at `len`, so the final (clamped) segment is only clean because the box was cleared first. Do not reintroduce `drawRGBBitmap` here — the manual blit is deliberate (see the memory-ownership notes above).

**Two kinds of layout, one drawing rule.** The **bar family** (styles 0-1) draws two strips where `bandsConf` says they are, so `rotateVU`, `align` and `boomboxVU` all matter to it, exactly as they always have. **Every other style** (2-6) draws into one area, `_cw` x `_ch`: time or frequency runs along its **width**, and the two channels split its **height** — except Spectrum Mirror, which splits its **width** — with `boomboxVU` ignored. Nothing transposes - the OLED, the rotated TFT box, the portrait TFT box and the BoomBox ribbon get the same picture. The booleans are read in `_draw()` for both cases, but only to size the area: `_rotate` picks `_cw = bandsConf.height` with `_ch = bandsConf.width * 2 + bandsConf.space`, otherwise it is the other way round. `_fillLocal()` is the only pixel helper; the non-bar painters never touch the per-channel geometry. A first cut transposed the non-bar styles onto the area's *long* axis, which put the portrait box's spectrum on its side and needed per-family special cases - all of that is gone.

**The sample styles need audio the VS1053 does not have.** `player.getWaveform(int16_t* out, uint16_t n)` returns `n` interleaved L/R pairs (oldest first) and `player.getSpectrum(uint8_t* bands, uint8_t n)` returns `2 * n` band values in 0..255 (L's bands then R's), or `false` when there is nothing to give. On the I2S path the audio task keeps a `VU_CAPTURE_SAMPLES` (512) rolling window, rotated into a published copy on the `f_vu` tick with a sequence counter around the copy; a reader that sees the sequence move drops the frame rather than drawing a torn trace. On the VS1053 both methods are stubs returning `false`, which is why the spectrum there is synthesised from the level in the widget - a fact the dropdown no longer states, since the label is the same on every backend. **The transform only exists where it is used**: `getSpectrum()` is dead code on a VS1053 build, so the linker drops its twiddle table, Hann table and FFT scratch entirely.

**The spectrum's scale is two factors that are easy to lose.** `getSpectrum()` returns 0..255 where 255 is a full-height bar, and it gets there as `sqrt(mean power) * (2/N) * (2/32768) * gain`. The first two are the transform's own normalisation; the third takes the int16 sample range down to 0..1 *and* gives back the Hann window's 0.5 coherent gain; and `gain` is **the meter's reference**, `clamp(256 / config.vuThreshold, 1, 32)`, so the loudest content the level meter has seen sits at full scale. That is what makes the I2S spectrum behave like the level bar beside it and like the simulated source on the VS1053, which is fed that same calibrated level. Dropping the `2/32768` pins every band at 255 and hot — that is exactly what the first hardware build did. The dB window is `VU_SPECTRUM_DB_FLOOR` (60) in `options.h`. The result is also **held**: a band rises at once and falls over `VU_SPECTRUM_FALL_MS` (400), because one 512-sample window is a noisy estimate - a band swings about 10 dB window to window, and a quiet window dims the whole display. That is the same hold that fixed a hardware flicker: the seqlock drops a frame whenever the publisher moves during the staging copy - **7% of frames**, ~1.8 a second, measured - and `getSpectrum()` used to report it as `false`, which a painter reads as "this backend has no transform" and answers by switching to its SIMULATED source for one refresh. On a sparse real spectrum that reads as the whole display suddenly filling in. A dropped frame now repeats the held frame; the strict check is what the waveform needs, where a spliced window is visible.

**Spectrum and trace layout details.** The inter-bar gap is `VU_SPECTRUM_SPACE_PX`, never `bandsConf.space` (that separates the two *meter strips* — 4 px on one TFT layout, 17 px on another, which left only 7 bars on the second). Because `barW` is floored, the bar block is **centred** — under the baseline in the stacked Spectrum, and inside each half in Spectrum Mirror, which is why its two low ends stop short of the divider, Spectrum Mirror reserving `th + 2` px more there so the line gets daylight on both sides instead of sitting flush against the first bar. And the Lissajous joins each sample pair to the previous one with a stepped line — one fill per pixel of the longer axis — because isolated dots read as a scatter rather than as a vectorscope trace.

**The sample styles use the same reference as everything else.** `_waveGain()` is `65536 / config.vuThreshold` in 8.8 fixed point (256 = unity, clamped to 0.25x..32x), applied to the waveform's amplitude and the Lissajous's X and Y, with the hot colouring measured *after* it. Without it both styles were absolute readings of raw int16: a trace that hugged the middle of the area and could never reach the hot zone. An uncalibrated meter (threshold 0) maps to unity. The Lissajous hot test is proportional on whichever axis the beam is furthest out on — a single `min(halfW, halfH)` scale can never go hot at the left or right edge of a wide box.

**The `[PSRAM]` core-monitor line is the memory diagnostic, and its fields are a contract.** `Used / total: Framebuffer: X, VU FFT: X, WebUI Cache: X, Audio buffered: X, Contiguous Free: X`. `VU FFT` is `vuPsramBytes` (declared in `config.h`, defined in `config.cpp`), written by the audio library's scratch allocator, and it reads 0 until a spectrum frame has been drawn because that allocation is lazy. `Contiguous Free` is `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` — the largest single free block, which is the figure that reveals fragmentation, since total free can look healthy while the biggest block shrinks. Add new fields at the end rather than reordering.

**Pacing is adaptive now, not a fixed divisor.** `VuWidget::loop()` times `_draw()` with `micros()`, smooths the cost (`_drawUs`, a quarter of each sample) and sets `interval = max(VU_REFRESH_MS, cost * VU_DUTY_FACTOR)`, both macros in `options.h` — at the default factor of 3, a frame spends at most a third of its own interval drawing. `VU_REFRESH_MS` is the floor and stays the number the level bars care about, so the limiter can only ever slow a style **below** the rate the audio core publishes levels at. `_redrawMs` (the limiter's timestamp) is deliberately **not** `_lastMs` (the fade maths' time base): while they were shared, a skipped frame showed up as a doubled `dt` in the fade. `VU_SAMPLE_REFRESH_DIV` is gone — it was a fixed guess at which styles were heavy and by how much, and it covered only two of them. On TFT the blit is inside `_draw()` and counted; on OLED the panel flush is paid afterwards by the display task, so the figure measured there is the widget's own cost. **While the startup services are downloading** the floor is multiplied by `VU_STARTUP_SERVICES_DIV` (4), so every style redraws a quarter as often for that window — the updater holds up to three TLS sessions on the network core and the audio stream is usually already up. The gate is [`startup.servicesBusy()`](src/core/startup.h:19), set by the services task around its own downloads: **not** `SVC_WILL_RUN`, which also covers the SD-mode park and the 10 s countdown, where the display must stay fast.

**History is the one style a variable redraw rate breaks**, because its x axis *is* time, so `_drawHistory()` advances by wall clock: it pushes however many columns the elapsed time owes (one per `VU_REFRESH_MS`), keeping the sub-tick remainder in `_histMs` so the strip does not drift, and a gap longer than the strip fills it in one go rather than replaying it. It also draws **two rects per column, not four** — the tick at each level is already inside either the fill (switch off) or the joined run (switch on), so only the first column, which has nothing to join to, needs bare ticks. The waveform and the Lissajous need neither fix: they plot the whole capture window every frame, so only their refresh rate moves.

**The draw-cost report is the measuring instrument.** With `WIDGET_DEBUG` defined, `loop()` accumulates frames, fills and draw time and prints them every 5 s like the Core Monitor: `[VU.Widget] Box 404x7, style 3: 28.1 FPS, draw 1.05ms avg / 2.31ms peak, interval 33ms, 12.0 fills/frame, free heap 93000`. `fills/frame` counts `_fillLocal()` and `_drawBand()` calls (not the blit) and is what shows whether a style is drawing rects it does not need; the draw time shows whether the box is simply too big for the panel. The members and the log are both behind `#ifdef WIDGET_DEBUG`, so a normal build pays nothing for either. The `WIDGET_DEBUG` line in `options.h`'s `ALL_DEBUG_LOGS` block used to read `#ifdef WIDGET_DEBUG`, a define that could never take effect; it is now `#ifndef`, matching its siblings.

### Colours on OLED

`config.theme.vumax` / `vumin` are set per driver, not via `dspcolors.h`:

- 1-bit panels (`tools/oledcolorfix.h`, `displayN5110.cpp`): both `TFT_FG`. A single bit cannot express two colours, so level reads from geometry alone.
- `OLED_GREYSCALE` with `core/options.h`, default `false` selects the driver's grayscale palette (instead of 1-bit mono), and is only valid for `SSD1322` (support removed from`SSD1327`)

`dspcolors.h` is deliberately minimal: only `BOOT_PRG_COLOR`, `BOOT_TXT_COLOR`, `TFT_BG`, `TFT_FG`. Panel-specific palettes live with their own drivers.

## Screen Rendering Fixes (Session: SH1106 YO_MONO)

### ClockWidget colon blink (widgets.cpp)
- The seconds-area `fillRect` at line 813 is guarded by `if (Clock_GFXfontPtr != NULL)` — its Y-position formula (`_top() - _timeheight + _space`) is only correct for special clock fonts; YO_MONO renders at `_top()` directly and the fillRect would clear into title rows.

### Screensaver clock boundaries (display.cpp, widgets.h)
- `minTop = max(TFT_FRAMEWDT, _clock->timeHeight())` — for framebuffer displays, `_config.top - _timeheight` must stay >= 0 to avoid framebuffer clipping above the display.
- `maxTop = dsp.height() - clockH - TFT_FRAMEWDT` — was missing the `-clockH` term, allowing the clock to render partially off-screen.
- Movement interval now uses `% SCREENSAVERMOVE` (default 5 seconds) instead of hardcoded `% 60`.
- `ClockWidget::timeHeight()` accessor added to expose `_timeheight`.

### NumWidget volume-digit clearing (widgets.cpp)
- YO_MONO clear height: `realth = _textheight * CHARHEIGHT` (was OLED-only; now applies to all displays with `Clock_GFXfontPtr == NULL`).
- Special-font clear height: `realth = _textHeight() + 1` — uses the font's actual glyph height + 1px margin, matching `_clearClock()`'s pattern.

### Font directory renamed
- `src/displays/ehfonts/` → `src/displays/clockfonts/`
- `YO_CLASSIC` removed as a clock font option (variable-width variant of YO_MONO that broke layout assumptions).

---

## Screen Rendering Fixes (Session: VU Rotated Layout)

- **New layout flag** `LayoutData::rotateVU` (exposed via `rotateVU_ptr`), treated exactly like `boomboxStyle` — absent means false. `VuWidget::_rotate` is read from `rotateVU_ptr` in `init()`.
- **Layout ordering** in `displayTFT480x320conf.h`: `_layoutNames` is now `Default`, `Default (VU Rotated)`, `BoomBox (VaraiTamas)`. The rotated layout is layout #2 (`bandsConf = { 32, 130, 4, 2, 10, 3 }`, `.rotateVU = true`); BoomBox moved to #3.
- **Blit choice**: `VuWidget::_draw()` uses the manual `startWrite()` / `setAddrWindow()` / `writePixels()` / `endWrite()` sequence for all three modes. `drawRGBBitmap()` was deliberately removed from the widget layer — the manual path depends only on `setAddrWindow` and `writePixels`, which every TFT driver is guaranteed to implement, and it issues a single bulk transfer rather than one `writePixels` call per scanline. Do not switch this back.
- **Direction**: the rotated VU fills left-to-right with `_vumaxcolor` at the right end.

## CPU Core Assignments & Stack Sizes

The ESP32 has two hardware cores: **Core 0** (PRO_CPU) and **Core 1** (APP_CPU). The ESP32 Arduino framework runs `setup()` and `loop()` on Core 1. Audio decoding is isolated on Core 0; all other application tasks run on Core 1.

### Compile-time Core Macros (`src/core/options.h`)

Two macros control core assignment across the codebase:

| Macro | Default | Valid override | Purpose |
|---|---|---|---|
| `AUDIO_CORE` | `0` | `1` | Core for audio decode task |
| `NETWORK_CORE` | `1` | `0` | Core for netserver and all network/utility tasks |
| `DSP_TASK_CORE_ID` | `1` | `0` | Core for the display loop task (independent of `NETWORK_CORE`) |

On single-core ESP32-C3 (`CONFIG_FREERTOS_UNICORE`), all three macros are forced to `0` automatically; defining any of them manually on a unicore build is a compile-time `#error`. `CONFIG_ASYNC_TCP_RUNNING_CORE` is tied to `NETWORK_CORE` so the AsyncTCP internal event task follows automatically.

### Board Stack Multiplier (`STACK_MULTIPLIER`)

A board-aware multiplier scales all five user-configurable FreeRTOS task stacks automatically:

| Board | `STACK_MULTIPLIER` | Effect |
|---|---|---|
| ESP32-S3 | 2 | All base stack sizes doubled |
| ESP32 | 1 | Base sizes unchanged |
| ESP32-C3 | 1 | Base sizes unchanged (C3 has *less* RAM than base ESP32) |

- Defined automatically after the board guard in `src/core/options.h`. Override in `myoptions.h` with `#define STACK_MULTIPLIER 1` or `2` if needed.
- Only values `1` and `2` are accepted — a compile-time `#error` fires otherwise.
- Per-task manual overrides (e.g. `#define DSP_TASK_STACK_SIZE 6`) bypass the multiplier; a `#elif` range guard validates the manually-set value.
- The multiplier applies **only** to the five configurable task stacks. Fixed-stack tasks (HTTPS workers, OTA) are unaffected.
- `SEARCHRESULTS_BUFFER` and `CONFIG_ASYNC_TCP_QUEUE_SIZE` use separate per-board explicit values (not `STACK_MULTIPLIER`) since they scale differently.

### Core 0 — Audio

#### `src/libraries/I2S_Audio/Audio.cpp` + `src/libraries/VS1053_Audio/audioVS1053Ex.cpp`
- Both audio libraries pin their `PeriodicTask` (audio decode loop) to `m_audioTaskCoreId`.
- `src/core/player.cpp` `Player::init()` calls `setAudioTaskCore(AUDIO_CORE)` to set this explicitly (defaults to Core 0).

### Core 1 — Everything Else

#### `src/core/display.cpp`
- `loopDspTask` ("DspTask") is pinned to `DSP_TASK_CORE_ID` (default `1`) via `xTaskCreatePinnedToCore`.
- This task calls `display.loop()` only. `netserver.loop()` was moved to its own dedicated task (see `netserverLoopTask` below).

#### `src/core/network.cpp`
- `doSync` (time/weather sync task) is pinned to `NETWORK_CORE`.
- `searchWiFi` (WiFi connection/retry loop) is pinned to `NETWORK_CORE`.
- `retryStreamConnection` (post-disconnect reconnect) is pinned to `NETWORK_CORE`.


#### `src/core/netserver.cpp` — `netserverLoopTask` + all utility tasks pinned to `NETWORK_CORE`
- `netserverLoopTask` (started by `NetServer::startLoopTask()`, called from `main.cpp` after each `netserver.begin()`) is pinned to `NETWORK_CORE`. It is the sole caller of `netserver.loop()`.
- All formerly scheduler-assigned (`xTaskCreate`) utility tasks are explicitly pinned to `NETWORK_CORE` via `xTaskCreatePinnedToCore`:
  `vTaskSearchRadioBrowser`, playback task (lambda), radio-browser click task (lambda), `checkForOnlineUpdateTask` (lambda), `startOnlineUpdateTask` (lambda)

#### `src/core/startup.cpp`, `src/core/commandhandler.cpp` — pinned to `NETWORK_CORE`
- `src/core/startup.cpp`: `startupServicesAsync`
- `src/core/commandhandler.cpp`: `vTaskFetchCuratedIndex`, `vTaskFetchCuratedPlaylist`

#### Arduino `loop()` — implicit Core 1
- All calls from `src/main.cpp` `loop()` run on Core 1: `telnet.loop()`, `battery.loop()`, `player.loop()`, `controls.loop()`.
- `netserver.loop()` is **not** called from `loop()` or from DspTask; it runs exclusively in `netserverLoopTask` pinned to `NETWORK_CORE`.

### FreeRTOS Task Reference

Stack sizes and priorities are controlled by macros in `src/core/options.h` (`/* Tweaks for Core Processes */`). Higher priority = more CPU; Arduino `loop()` runs at priority 1. Priority 0 is idle-level (starved) and is never used. Per-task local conversion macros (`_BYTES`) are defined at the top of each `.cpp` file (except `SET_LOOP_TASK_STACK_SIZE` which is an `Arduino.h` macro). Stack defaults scale with `STACK_MULTIPLIER` — values shown as ESP32/C3 (1x) / S3 (2x).

| Task | File | Stack macro (default ESP32 / S3) | Priority macro (default) | Notes |
|---|---|---|---|---|
| `loopTask` | main.cpp | `LOOP_TASK_STACK_SIZE` KB (8 / 16) | 1 (framework) | Arduino loop(); `SET_LOOP_TASK_STACK_SIZE()` applies at boot |
| `DspTask` | display.cpp | `DSP_TASK_STACK_SIZE` KB (4 / 8) | `DSP_TASK_PRIORITY` (2) | — |
| `netserverLoopTask` | netserver.cpp | `NETSERVER_TASK_STACK_SIZE` KB (4 / 8) | `NETSERVER_TASK_PRIORITY` (2) | — |
| `doSync` | network.cpp | `NETWORK_TASK_STACK_SIZE` KB (4 / 8) | `LOW_TASK_PRIORITY` (1) | Time/weather sync |
| `searchWiFi` ×2 | network.cpp | `NETWORK_TASK_STACK_SIZE` KB (4 / 8) | `NET_TASK_PRIORITY` (3) | — |
| `retryStreamConnection` | network.cpp | `NETWORK_TASK_STACK_SIZE` KB (4 / 8) | `NET_TASK_PRIORITY` (3) | Post-disconnect reconnect |
| `retryStreamConnection` | player.cpp | `NETWORK_TASK_STACK_SIZE` KB (4 / 8) | `NET_TASK_PRIORITY` (3) | Stream drop reconnect; was hardcoded to Core 0 (bug) |
| `vTaskFetchCuratedIndex/Playlist` | commandhandler.cpp | 8192 fixed | `LOW_TASK_PRIORITY` (1) | HTTPS — stack hardcoded |
| `vTaskSearchRadioBrowser` | netserver.cpp | 8192 fixed | `LOW_TASK_PRIORITY` (1) | HTTPS — stack hardcoded |
| `playbackTask` (lambda) | netserver.cpp | 4096 http / 8192 https | `PLAYBACK_TASK_PRIORITY` (3) | Dynamic stack based on URL scheme |
| `rbClickTask` (lambda) | netserver.cpp | 8192 fixed | `LOW_TASK_PRIORITY` (1) | HTTPS — stack hardcoded |
| `checkForOnlineUpdateTask` (lambda) | netserver.cpp | 8192 fixed | `LOW_TASK_PRIORITY` (1) | HTTPS — stack hardcoded |
| `startOnlineUpdateTask` (lambda) | netserver.cpp | 16384 fixed | `NET_TASK_PRIORITY` (3) | OTA — stack hardcoded |
| `startupServicesAsync` | startup.cpp | 8192 fixed | `LOW_TASK_PRIORITY` (1) | HTTPS — stack hardcoded |

### CORE_MONITOR debug feature (opt-in)

Enable by adding `#define CORE_MONITOR` to `myoptions.h`. Zero impact on binary when not defined.

When active, emits a `FUNCTIONLOG("Core Monitor", ...)` line to serial+telnet every 5 seconds:
- **Dual-core output**: `Core0(+Audio) loops/s: 82 (12.15ms/loop) | Core1(Main+Net+TCP+Disp) loops/s: 15327 (0.07ms/loop) | MaxMainLoopUs: 5197 | Heap: 163732`
  - The labels in parentheses are built at compile time via `CORE_0` / `CORE_1` string macros defined in `options.h`. Each macro concatenates component tokens (`+Audio`, `+Net`, `+TCP`, `+Disp`) conditioned on where `AUDIO_CORE`, `NETWORK_CORE`, `CONFIG_ASYNC_TCP_RUNNING_CORE`, and `DSP_TASK_CORE_ID` are assigned. These macros are only defined when both `CORE_MONITOR` and `!CONFIG_FREERTOS_UNICORE` are true.
- **Unicore C3 output**: `Core0 loops/5s: N (worst: N) | Core0(Main) loops/5s: N (worst: N) | MaxMainLoopUs: N | Heap: N` — `CORE_0`/`CORE_1` are not available on unicore; labels are static strings
- **Also shows LittleFS information**: `Used: N / N bytes, Free: N bytes`

Implementation:
- `src/core/display.cpp`: `volatile uint32_t cmDspLoopCount` incremented each `loopDspTask` iteration
- `src/main.cpp`: `extern` reference to `cmDspLoopCount` + per-loop timing via `micros()`; worst-case counters are all-time minimums (never reset between windows)

---

## Hardware-Specific Notes (High-Risk Paths)

This section calls out hardware implementations that diverge from the common code path and are more likely to regress.

## Audio backend variants (`src/core/player.cpp` + `src/libraries/*`)
- Two major audio stacks are used depending on macros/hardware:
  - I2S audio library path
  - VS1053 external decoder path
- `options.h` enforces that both are not enabled together.
- Risk notes:
  - behavior differences between backends (metadata timing, codec handling, volume behavior) can create env-specific bugs.
  - some callback/metadata handling is shared while low-level decoder behavior is not.

## Display driver families (`src/displays/display*.cpp/.h`)
- Most drivers implement similar APIs but capability differences exist:
  - sleep/wake/invert support varies
  - color depth and text rendering differ
  - touch coupling only exists for certain panel combinations
- Risk notes:
  - UI assumptions tested on one controller may fail on another due to geometry, fonts, or refresh behavior.
  - conf-layout headers can hide clipping/overlap issues until specific display targets are built.

## Touch controllers (`src/core/touchscreen.cpp`)
- Multiple controller backends (XPT2046, GT911, FT6336) with shared gesture mapping.
- Risk notes:
  - orientation/flip and calibration behavior can diverge by controller.
  - long-press/swipe thresholds can feel different across hardware even with same app logic.

---

## Custom Libraries (`src/libraries`)

These are **not** third-party packages installable via PlatformIO's registry. They are custom or heavily-modified libraries embedded directly in the repository, mostly inherited from yoRadio and extended for ehRadio. Consult `src/libraries/libraries-note.md` for the origin, upstream source, and modification status of each library.

### Display driver libraries
- `Adafruit_GC9106Ex/` — GC9106 TFT driver (not a real Adafruit library; adapted from prenticedavid)
- `Adafruit_ST7796S/` — ST7796S TFT driver (same origin)
- `ILI9225Fix/` — ILI9225 TFT driver (heavily modified)
- `ILI9488/` — ILI9486/ILI9488 SPI driver (modified from ZinggJM)
- `LiquidCrystalI2C/` — I2C LCD driver (slightly modified from johnrickman)
- `SSD1322/` — SSD1322 OLED driver (slightly modified from JamesHagerman)
- `ST7920/` — ST7920 GLCD driver

### Audio decoder libraries
- `I2S_Audio/` — software I2S audio decoder (adapted from schreibfaul1/ESP32-audioI2S via Maleksm's yoRadio mod). PSRAM buffer size now configurable via `PSRAM_BUFSIZE` macro.
- `VS1053_Audio/` — VS1053 hardware decoder driver (adapted from schreibfaul1/ESP32-vs1053_ext via Maleksm's yoRadio mod). PSRAM buffer size now configurable via `PSRAM_BUFSIZE` macro. `stopSong()` SM_CANCEL sequence now guarded by `if(m_f_running)` — prevents permanently stuck CANCEL bit when stop is called during init with no song playing. `VS_PATCH_ENABLE` forced `false` on this hardware — FLAC patches produce audio silence on this VS1053 variant.
- `ES8311_Audio/` — ES8311 codec driver (written for ehRadio by kasperaitis)

### Touchscreen library
- `FT6336_Touchscreen/` — FT6336 capacitive touch driver (written for ehRadio by kasperaitis)

### Logging integration in custom libraries
- Selected library-level diagnostic prints now use centralized logging macros for consistency with core logs:
  - `VS1053_Audio/audioVS1053Ex.cpp` (VU meter status/error)
  - `ES8311_Audio/es8311.cpp` (register dump helper)
  - `FT6336_Touchscreen/FT6336.cpp` (startup probe log)
  - `ILI9225Fix/TFT_22_ILI9225Fix.cpp` (`DEBUG` macro print path)

### Include conventions in library files
- Library `.cpp` files that reference project defines begin with `#include "../../core/options.h"` as the **first line** (before any `#if` guard), then gate all remaining includes and code behind the relevant `#if` condition (e.g., `#if DSP_MODEL==DSP_ST7920`, `#if defined(USE_AUDIO_I2S) || defined(USE_AUDIO_ESP32_DAC)`, `#if defined(USE_AUDIO_VS1053)`). This pattern is acceptable and intentional.
- Library `.h` files do not include `options.h`; they are self-contained and guarded with `#ifndef`/`#pragma once`.

---

## Locale and Translation Map (`src/locale`)

## `src/core/locale.h` (selector)
- Thin include wrapper: includes `dsplocale.h` and defines `WEBUI_LOCALE` from `DSP_LOCALE` if not overridden.
- `_activeLocale` runtime index (set from `config.store.locale_display`), `l10n()` / `l10n_dow()` / `l10n_month()` / `l10n_wind()` helpers.
- `l10n_findLocale()` resolves locale code to array index at runtime.

## Display locale files (`src/locale/display/*.json`)
- 36 JSON source files (one per locale), compiled via `make_dsplocale.py` into `dsplocale.h` PROGMEM mega-header.
- Master key set defined by `en_US.json` (67 keys: days, months, wind, weather, status labels).
- `static_assert` validates `DSP_LOCALE` at compile time against known locale codes.
- `dsplocale_index` PROGMEM string served at `/dsplocale.json` for WebUI dropdown.

## WebUI locale files (`src/locale/www/*.json`)
- 50 JSON source files, compiled via `make_wwwlocale.py` into `wwwlocale.h` PROGMEM (gzip-compressed byte arrays).
- Served from PROGMEM at `/locale.json` (with `Content-Encoding: gzip`) and `/wwwlocale.json` (index).
- No LittleFS files needed — all locale data is compile-time embedded.

## Locale build tools
- `src/locale/make_dsplocale.py`: validates display JSONs, generates `dsplocale.h` with PROGMEM string tables + enum.
- `src/locale/make_wwwlocale.py`: validates webui JSONs, gzip-compresses into `wwwlocale.h` PROGMEM byte arrays.
- `src/locale/hardcode_locale_to_webui.py`: bake locale text into WebUI assets (for `HARDCODED_WEBUI_LOCALE`).

## Locale maintenance tools
- `src/locale/www_tool.py` (was `scan_www_check_json.py`): scan HTML/JS for i18n keys, check/add/translate/sort www locale JSONs. Supports `--create`.
- `src/locale/display_tool.py` (NEW): manage display JSONs against master. Sort uses master key order (never alphabetizes). Clean never touches master. Supports `--create`.
- `src/locale/trans_deepl.py` (was `scan_trans_deepl.py`): DeepL translation module. Uses `trans_*.key` discovery pattern (was `scan_trans_*.key`).
- `src/locale/trans_deepl.md` (was `scan_trans_deepl.md`): DeepL setup + usage docs.

---

## WebUI <-> `config.store` Integration Playbook (Critical Section)

This section is specifically for adding/removing settings and avoiding missed linkage points.

## When adding a new runtime setting field

1. Add macro default in `src/core/options.h` (and optionally override in `myoptions.h`).
2. Add field in `config_t` in `src/core/config.h`.
3. Add key mapping in `Config::keyMap` in `src/core/config.cpp`.
4. Add reset behavior in `Config::defaultSettings(...)` branch (the right group).
5. Add getter payload in `netserver.processQueue()`:
   - whichever `GET*` JSON block should include it (`GETSYSTEM`, `GETSCREEN`, etc.).
6. Add command handling in `src/core/commandhandler.cpp`:
   - parse command key
   - persist with `saveValue(...)`
   - trigger display/network side effects and request updates as needed.
7. Add WebUI wiring:
   - element in `data/www/settings.html` with id and `data-command`.
   - fallback label text + `data-i18n` key.
   - add i18n key in `src/locale/www/en_US.json` (and optionally others).
8. Ensure websocket UI apply path exists in `data/www/script.js`:
   - `setupElement(...)` supports element type/id.
   - incoming `GET*` payload key matches DOM element id or custom handler.
9. If setting is locale/time/weather related, update `data/www/settings.js` apply handlers too.
10. Telnet command handling is thin-dispatch by default: update `src/core/commandhandler.cpp` first, and only extend `src/core/telnet.cpp` if protocol normalization needs a new alias/form.
11. If setting affects startup behavior, check `main.cpp`, `config.init()`, and `startup.startupServices()`.
12. Update this `code-summary.md`.
13. Update `Commands.md`.

## When removing a setting field

1. Remove/disable command usage in `commandhandler.cpp`.
2. Remove from `GET*` payload in `netserver.cpp`.
3. Remove UI controls and JS references.
4. Remove from `config_t` + `keyMap`.
5. Add removed key to `Config::deleteOldKeys()` if old persisted value should be cleaned.
6. Remove locale keys from `src/locale/www/en_US.json` (and regenerate/check).
7. Check telnet/mqtt code paths for orphan logic.
8. Update this file.

## Why changes are often missed

Frequent miss points:
- `Config::defaultSettings(...)` sections
- `Config::keyMap` update
- `GET*` outbound payloads in `netserver`
- DOM id mismatch vs websocket payload key
- locale keys missing from `en_US.json` and `data-i18n`
- telnet parity for advanced settings

---

## Cross-Link Matrices

## Matrix A: Settings request/response paths

- Browser asks for settings:
  - `script.js` sends `getsystem=1` etc.
  - `commandhandler.cpp` -> `netserver.requestOnChange(GETSYSTEM, cid)`
  - `netserver.cpp` builds JSON from `config.store`
  - `script.js` maps keys to DOM by id

- Browser applies settings:
  - UI emits `key=value` over websocket
  - `netserver.onWsMessage()` -> `cmd.exec()`
  - `cmd.exec()` updates `config.store` and side effects
  - server emits follow-up updates where needed

## Matrix B: Same behavior surface via different channels

- WebUI: `commandhandler.cpp`
- HTTP URL params: `netserver.cpp` `handleIndex()` multi-param loop -> `commandhandler.cpp` (with source blocklist)
- Telnet/serial: normalized parser -> `commandhandler.cpp` (minimal local handling)
- MQTT: normalized payload parser -> `commandhandler.cpp` (with source blocklist)
- Physical controls: `controls.cpp`

Implication:
- For universal control behavior, update `commandhandler.cpp` first; then adjust only channel-specific parser aliases/blocklists.

## Matrix C: Playlist actions

- Edit/import in browser -> upload to `/webboard` -> LittleFS write
- `netserver` triggers `PLAYLISTSAVED`
- `config.indexPlaylist()/initPlaylist()` refresh index
- `player` and display refresh via request queue events

---

## Telnet Section Interactions (Explicit)

`telnet.cpp` primarily acts as a command ingress path now. Most command behavior is owned by `commandhandler.cpp` and shared with MQTT/HTTP paths.

Input-line handling is delimiter-based (`\r` or `\n`) with explicit 2000 ms timeouts, which avoids delayed command execution on clients that submit CR without LF.

If you add a setting command in `commandhandler.cpp`, telnet and MQTT generally inherit it automatically unless blocked by the shared HTTP/MQTT/Telnet non-WebUI policy.

---

> Tracked issues and risk notes are in `.github/code-issues.md`.


