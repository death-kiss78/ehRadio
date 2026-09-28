# ehRadio AI Coding Guide

## Note

- **This file**: If you wish to NOT use this file, feel free to add it to your own private exclusions (`.git/info/exclude`).  Please do not try to sync your own version of this file back in a PR to `ehRadio:dev`.
- **code-summary.md**: The ehRadio `Bible` may be hard for a human to read but provides a good (if lengthy) summary of the codebase to be more easily digested by an AI / LM Copilot.

## About

- **ehRadio**: A fork of [yoRadio](https://github.com/e2002/yoradio) with added Web UI functionality, improved Web UI, usability, and customization.
- **Arduino**: Trip5 hosts firmware files, flashing tools, and Releases on the [Github Repo](https://github.com/trip5/ehRadio) and [Github Page](https://trip5.github.io/ehRadio/)
- **Online Flasher**: Releases are also available through the [ESP Web Tool](https://trip5.github.io/ehRadio/firmware.html)
- **Online Updating**: Files on a running device are updated using ESPFileUpdater from the Releases on the Github Repo.

## ⚠️ Critical Rules

> **These rules are HARD CONSTRAINTS, not guidelines. Violating any rule is an error.**
> Before making ANY edit or suggesting code, mentally validate against ALL 5 rules. If any rule would be violated, **STOP** and inform the user. When in doubt, ASK.

**Rule #1**: **Plan Mode for Large Changes**
- For changes spanning **more than 50 lines** (summed across ALL files in the change), you **MUST STOP** and inform the user:  
  > "This change is large. Please switch to Plan mode so we can review a plan before making edits."  
- Only proceed if the user explicitly confirms or switches to Plan mode.
- In Plan mode, once the user states "start implementation" (or equivalent), **implement exactly as agreed**. Do **NOT re-confirm or reinterpret** the scope — follow the finalized plan.
- If you are uncertain about the total line count, **ASK** before proceeding.

**Rule #2**: **One File/Simple Changes Exception**
- This is the **ONLY** exception to Rule #1. There is no "small multi-file" loophole. You must satisfy **ALL 3 points**:
  - Changes are **less than or equal to 50 lines** (summed across all files).
  - The changes affect **only one file or one logical set of files** (e.g., a `.cpp` file and its matching `.h` file).
  - The user has given **explicit instructions** on what to modify.
- **Self-check**: Before every edit, ask yourself: *"Am I touching exactly one file (or one .cpp+.h pair)?"* If not, Plan mode is required regardless of line count.

**Rule #3**: **Restricted Configuration Files**
- Do **NOT** edit or even **suggest** edits to the following files unless the user provides **explicit confirmation**:
  - `myoptions.h`
  - `src/core/options.h`
  - `platformio.ini`

**Rule #4**: **Code Interaction Documentation**
- If your change affects **code interactions**, external APIs, storage keys, or external contracts:
  - Ensure `.github/code-summary.md` is updated in the **same change set**.
- **Bug fixes** do not require updates unless they modify external contracts.

**Rule #5**: **Pre-edit Research Required**
- For any non-trivial change (beyond an isolated one-line fix), first determine whether it touches the firmware:
  - **Firmware files** = any `*.c`, `*.cpp`, `*.h` file anywhere in the repo, plus anything under `src/`, `libraries/`, or `data/`.
  - **If the change touches ANY firmware file**: You **MUST READ** `.github/code-summary.md` before writing code. Also check `.github/code-issues.md` for known issues in the affected area.
  - **If the change touches ONLY non-firmware files** (workflows, docs, build scripts, config generators, Home Assistant, images, etc.): `code-summary.md` review is **NOT** required. Still check `code-issues.md` if relevant.
  - **One-line/trivial fixes** (typos, formatting) are exempt regardless of file type.
- **What `.github/code-issues.md` is for**: it tracks **open** issues that still need investigation, or that are blocked — for example, waiting on hardware the maintainer does not own. It is **not** a changelog.
  - Do **not** add entries for problems found and fixed in the same change set.
  - Do **not** add general rules or subsystem documentation there; that belongs in `code-summary.md` (Rule #4).
  - Do read it before editing an affected area, and **update or close an existing entry** if your change fixes it or alters the code that entry describes.

**Enforcement and AI Behavior**
- Always validate proposed changes against these rules **before every action**.
- For Rule #1 and Rule #2 violations: Explain which rule is violated and wait for user confirmation.
- Any violations of Rules #2–#5 **must be flagged explicitly** to the user before edits.
- **Pre-action checklist** — mentally answer before writing code:
  1. Total lines across all files > 50? → Plan mode required (Rule #1)
  2. More than 1 file (not a .cpp/.h pair)? → Plan mode required (Rule #2)
  3. Touching `myoptions.h`, `options.h`, or `platformio.ini`? → Get explicit confirmation (Rule #3)
  4. Affecting external contracts/APIs/storage keys? → Update `code-summary.md` (Rule #4)
  5. Touching any firmware file (`*.c`, `*.cpp`, `*.h`, `*.ino`, `src/`, `libraries/`, `data/`)? → Read `code-summary.md` first (Rule #5)

## Project Structure
- **Config Cascade**: `platformio.ini` (env #define) → `myoptions.h` (hardware profile, user defaults) → `options.h` (fallback defaults for anything undefined). Third-party libraries are their own translation units and never see this cascade: the few values they must agree with us about live in `src/core/options_overrides.h`, which `platformio.ini` force-includes into every TU.
- **Core logic**: `src/core/` (Player, Display, Network, Config, Controls).
- **Headers that own data**: `src/locale/dsplocale.h` defines its locale tables (`l10n_strings[36][77]` and every translated string) as namespace-scope `const` arrays *inside the header*, so each translation unit that includes it embeds another complete copy - roughly 48 KB of flash. Do not add that include to a new file: call `l10n(...)` from a file that already has it, or expose a small accessor as `Player::isConnecting()` does for the "connecting" placeholder check.
- **Libraries path**: Software codecs: `libraries/I2S_Audio/`, `libraries/ES8311_Audio` / Hardware decoder: `libraries/VS1053_Audio/` (Hardware chip), other folders are custom drivers for other display, touchscreen, and other hardware.
- **UI**: Widgets in `src/displays/widgets/`, drivers in `src/displays/`.
- **Plugins**: The former yoRadio plugin hook system has been removed. Add new behavior in the owning core module instead of reviving plugin-style hooks.
- **Web UI**: Most files in `data/www` are served with headers in `src/core/netserver.h`. `search.html` and `curated.html` are not.

## Functionality
- **Hardware**: The firmware is built according to the hardware that is connected to it and users who will use it.  These are defined by files listed in Config Cascade.
- **Software**: `src/core/options.h` and the Config Cascade should be used to extend functionality, not limit it. `#if defined` and `#ifndef` should not be used in the code for configuration not related to hardware.
- **Granular Control in Web UI**: If not hardware-related, functionality should be changeable in the Web UI, not controlled by a `#define` in Config Cascade.

## Force-Included Config (`src/core/options_overrides.h`)

`[ehradio] build_flags` in `platformio.ini` passes `-include src/core/options_overrides.h`, so that header is compiled first in **every** translation unit: our sources, every managed library (AsyncTCP, ESPAsyncWebServer, Adafruit, ...), and the framework's own `.c` files.

- **Never force-include `options.h`.** It is not a library-safe header: it does `#include <SPI.h>`, and a library TU's compile line does not carry the SPI include path - `Wire.cpp` fails with `options.h:273:10: fatal error: SPI.h: No such file or directory` - and it uses C++-only `static_assert` while `build_flags` also apply to `.c` files. `options_overrides.h` exists to hold the few library-visible values instead.
- **Keep it dependency-free**: preprocessor only, no framework or library includes, no types, no C++ syntax, and only values a library must agree with us about. The framework's `.c` files compile through it, so C compatibility is a hard requirement.
- **`myoptions.h` stays the user channel**: the header includes it first, so a value set there beats every default in the header - that is the supported way to set `CONFIG_ASYNC_TCP_USE_WDT`, `CONFIG_ASYNC_TCP_RUNNING_CORE`, and so on. This holds only in builds that carry the force-include: root `platformio.ini` and `builds/trip5/platformio.ini` do, the other `builds/*` templates do not.
- **The `NETWORK_CORE` tree is duplicated here and in `options.h` on purpose, and both copies must survive being evaluated twice** - this header is compiled first, so `options.h` always meets the value already set. That needs no marker macro: the dual-core branch is wrapped in `#ifndef NETWORK_CORE`, and the unicore branch only errors when the value it finds is not 0, so it cannot mistake our definition for a user's. Keep the two trees identical when editing either.
- **A value change here may not rebuild libraries**: after the header was introduced, only `src/` objects were recompiled. Use `pio run -t clean -e <env>` when a value change must be guaranteed to take effect.
- **Any long card operation reachable from a web handler must feed the task watchdog** (`sdFeedWatchdog()`), because web handlers run in AsyncTCP's task and that task is subscribed (`CONFIG_ASYNC_TCP_USE_WDT 1`). The subscription is kept on deliberately: it turns a handler wedged on a stalled SD call into an automatic reboot instead of a device that needs a power cycle.
