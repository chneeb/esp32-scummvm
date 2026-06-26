# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A fork of [ScummVM](https://github.com/scummvm/scummvm) targeting the **ESP32-P4**. The ESP32-specific backend lives entirely under `backends/platform/esp32/`. Everything else is upstream ScummVM.

**Actual hardware in use:** a **Waveshare ESP32-P4 Module Basic Kit** (ESP32-P4-Module-DEV-KIT), built against Espressif's `esp32_p4_function_ev_board` BSP. The boards are not identical — notably the Waveshare baseboard routes USB through a mux + onboard hub (see "USB keyboard" below), which the Espressif BSP does not handle.

The board is configured for a **5" 800×480 MIPI DSI display** (RPi 7" V1-style clone, connected via the 22-pin Pi-style FPC cable that came with the display), NOT the native 1024×600 EK79007 panel. The panel is driven by a **TC358762 DSI-to-DPI bridge** + **ATTINY88** power/control MCU (I2C 0x45, firmware 0xC3); touch IC is FT5x06 at I2C 0x38. ScummVM boots and runs fully — display, touch, SD card, and audio all work (see "Display" below). Keyboard input is via an **M5Stack CardKB I2C keyboard** on the Qwiic/I2C port (see "Keyboard" below); a **USB HID keyboard does NOT work on this board** (ESP-IDF limitation behind the onboard hub).

## Build system overview

There are **two nested build systems**:

1. **ESP-IDF (outer)** — run from `backends/platform/esp32/` using `idf.py`. Manages hardware drivers, FreeRTOS tasks, flash/partition layout, and managed components.
2. **ScummVM Makefile (inner)** — invoked by ESP-IDF as an `ExternalProject` (defined in `components/scummvm/CMakeLists.txt`). Runs `./configure --host=esp32` then `make libs` in the repo root, producing static `.a` libraries that ESP-IDF links into the final binary.

The ScummVM configure script has an `esp32` host target (around line 3620 of `configure`) that hard-codes include paths for managed components (zlib, ogg, libpng) and sets `_port_mk="backends/platform/esp32/esp32.mk"`.

## Building

Activate the IDF environment first (must be done in every shell session):
```bash
. ~/esp/esp-idf/export.sh
```

Then from `backends/platform/esp32/`:
```bash
idf.py build       # build only
idf.py flash       # build + flash
idf.py monitor     # serial monitor
idf.py flash monitor  # flash then monitor
```

The final binary is >10 MiB. App partition is 15 MB; current build uses ~70% of it.

## Forcing a ScummVM reconfigure

The inner `./configure` only re-runs when its stamp file is present. After changing `configure` or `components/scummvm/CMakeLists.txt`, force a reconfigure:
```bash
rm backends/platform/esp32/components/scummvm/src/scummvm_build-stamp/scummvm_build-configure
```

To force a full ScummVM rebuild (not just reconfigure):
```bash
rm backends/platform/esp32/components/scummvm/src/scummvm_build-stamp/scummvm_build-build
```

## Enabling/disabling game engines

Edit `backends/platform/esp32/components/scummvm/CMakeLists.txt`:
- `ENGINES` — main engine names (e.g. `scumm agi sci`)
- `SUBENGINES` — sub-engine variants (e.g. `scumm-7-8 he sci32`)

Find available engine names with `./configure --help` from the repo root. After changing engines, delete the build stamp and rebuild.

## Key source files (ESP32 backend)

All under `backends/platform/esp32/main/`:

| File | Role |
|---|---|
| `main.cpp` | `OSystem_esp32` — the ScummVM platform backend; event loop, task setup |
| `esp-graphics.cpp/h` | `EspGraphicsManager` — double-buffered framebuffer via ESP LCD panel + PPA hardware scaler |
| `esp-mixer.cpp/h` | Audio output via FreeRTOS ring buffer |
| `usb_hid.c` | USB Host HID driver — keyboard and mouse input via `espressif__usb_host_hid` |
| `mmc.c` | SD card mount via `sdcard_mount_blkcache` |
| `posixesp-fs-factory.cpp` | POSIX filesystem abstraction over SD card |

Custom ESP-IDF components under `components/`:
- `blkcache` — block-level read cache sitting between SD card and FATFS
- `hid_ev` — HID report descriptor parser; converts raw USB HID reports to `hid_ev_t` events

## libpng include path fix

`configure` line ~3631 sets `PNG_CFLAGS` for the esp32 host. `pnglibconf.h` lives in `managed_components/espressif__libpng/` (not in the `libpng/` subdirectory), so both paths must be included:
```
PNG_CFLAGS="-Ibackends/platform/esp32/managed_components/espressif__libpng/libpng -Ibackends/platform/esp32/managed_components/espressif__libpng"
```
This is already applied; note it if the configure script is ever regenerated or rebased from upstream.

## SD card / runtime content

The SD card content has already been generated and lives at `esp32dist/scummvm/`. Copy that folder to the root of a FAT-formatted micro-SD card. Game folders can go anywhere on the card.

To regenerate after engine or theme changes, run from the repo root:
```bash
make esp32dist
```

## "Skipping optional dependency: espressif/usb"

This is expected and harmless. The `espressif/usb` external component is only needed for IDF ≥ 6.0; IDF 5.x includes the USB Host stack in-tree. (USB keyboard does not work on the current Waveshare board for an unrelated reason — see "USB keyboard" below.)

## Display (working)

The display, touch, audio, and SD all work (keyboard is via I2C CardKB — see "Keyboard"). The panel is an **RPi 7" V1-style 800×480 DSI** driven by a **TC358762 DSI-to-DPI bridge** + **ATTINY88** MCU (I2C 0x45, fw 0xC3), touch **FT5x06** at I2C 0x38. It is NOT an ILI9881C/HX8399 panel — those vendor-init paths were tried and stayed dark; only the TC358762 bridge path produces an image.

### Reference driver

Ported from `github.com/embenix/ESP32-P4-DSI-Support-Hub` → `main/lcd_config/rpi-7inch-touch-display-v1.c` (repo default branch is **master**). The `Luckfox-5inch` config in that repo is a *different* chip (ILI9881C) — do not use it for this hardware.

### How it's driven (key files)

- **`bsp_override/esp32_p4_function_ev_board_noglib/esp32_p4_function_ev_board.c`** — `#define BSP_LCD_PANEL_TC358762` selects the bridge path. (This is our forked BSP — see "Note: managed_components" below. The matching `managed_components/...` copy is gitignored and unused.) ATTINY88 power-on, then a raw MIPI-DSI **DPI** panel (no panel-IC driver), then `tc358762_bridge_init()` writes the bridge registers via DSI Generic Long Writes (LP during blanking) after `esp_lcd_panel_init()` + continuous-HS clock. DSI: 1 data lane, 600 Mbps, NON-BURST with sync pulses. Timing: 800×480, HBP46/HSW2/HFP210, VBP4/VSW20/VFP22, 26 MHz.
- **`main/esp-graphics.cpp`** — double-buffered DPI framebuffer, PPA scaler, FT5x06 touch at 0x38.

### Three things that had to be right (don't regress these)

1. **RGB888 end-to-end** (fixes a ~200px seamless horizontal wrap that RGB565 caused — a TC358762 FIFO read/write phase issue, *not* a sync/porch issue, so HBP/HFP changes never fix it and large ones blank the panel). Set in the BSP: `TC_LCDCTRL = 0x00100150` (RGB888 bit) and DPI `pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888`. In esp-graphics.cpp the framebuffer is 3 bytes/px and PPA output is `PPA_SRM_COLOR_MODE_RGB888`.
2. **R/B byte order = B,G,R** in the framebuffer. The **direct-write** paths (overlay 565→888, loading-screen write, clearOverlay) write/read bytes as B,G,R. The **game path** (CLUT8 → pal16 RGB565 → PPA → RGB888) does **NOT** swap — PPA's 565→888 expansion already produces B,G,R. (If both swap, in-game colors invert e.g. skin→blue while the menu stays correct — that asymmetry is the tell.)
3. **Touch** flags `mirror_x=1, mirror_y=1, swap_xy=0` in the FT5x06 config.

### Workflow notes

- **No need to `rm` the managed-component `.obj` before building** — ninja tracks edits correctly (verify with `ninja -n` in `build/`). The old "build cache bug" was a misdiagnosed stale flash.
- **Power-cycle is only needed to recover from a bad/dark build** that wedged the bridge's PLL (the RTS reset doesn't power-cycle the display, which draws power from its own connector). A good build flashes + RTS-resets fine without unplugging. Could be hardened in `tc358762_power_on` (longer reset hold / true power-down) if it ever gets annoying.

## Keyboard

**Working: M5Stack CardKB I2C keyboard** (plugged into the board's Qwiic/I2C port). Implemented in `OSystem_esp32::pollEvent` / `cardkb_map` (`main/main.cpp`): the device is at I2C **0x5F** and returns a single byte = the pressed key's code (`0` = nothing). It's polled at ~60 Hz on the shared I2C bus (`bsp_i2c_get_handle()`) and edge-detected into KEYDOWN/KEYUP pairs (the CardKB only reports the current key, not press/release). Mapping (per the CardKB firmware `key_map`):
- printable keys, Shift (uppercase/symbols) and Sym layers → their ASCII directly
- enter=13, backspace=8, tab=9, esc=27, shift+del=0x7F→Delete
- arrows: Left=180, Up=181, Down=182, Right=183
- **Fn layer (bytes 128–175): `Fn+1..Fn+0 → F1..F10`, `Fn+del → F11`, `Fn+tab → F12`** — so **F5 (save) = Fn+5**, **F7 (load) = Fn+7**.

Also available: the **on-screen keyboard** (two-finger tap) as a touch-only fallback.

### USB keyboard does NOT work on this board

The USB HID code is fully compiled, linked and started (`usb_hid_task()` in `app_main`, `main/usb_hid.c`; host stack logs `Waiting for HID Device to be connected`). It's blocked by the **Waveshare USB topology + ESP-IDF limits**, not by our code:

- The board routes the ESP32-P4 **high-speed** USB through an **FSUSB42 mux** to either an onboard **CH334F 4-port hub** or a direct connector (J8), selected in hardware (`USB_SEL` via R37 + the **H3 header/jumper** — *not* a GPIO, so firmware can't switch it).
- The Type-C port labelled **"USB" is the ESP32-P4 full-speed OTG** (GPIO24/25), a different controller; ESP-IDF only supports the **HS/UTMI host** on P4 (`usb_host.c`: "We support HS+UTMI only ATM.").
- Even with the hub enumerating, a keyboard is rejected: `HUB: Connected device is LS, transaction translator (TT) is not supported` — ESP-IDF v5.5 has **not implemented the hub Transaction Translator** (`ext_hub.c`), so Low/Full-Speed devices behind the HS hub can't be used.

Net: a USB keyboard would only work wired directly to the HS root port (no hub); the hub ports and the FS "USB" port are dead ends. The CardKB above is the practical keyboard, so this is parked.

## Note: forked BSP under `bsp_override/` (managed_components is gitignored)

`backends/platform/esp32/.gitignore` ignores `managed_components/`. Our display fixes would be lost there, so the BSP component `esp32_p4_function_ev_board_noglib` is **vendored, git-tracked**, at `backends/platform/esp32/bsp_override/esp32_p4_function_ev_board_noglib/` and wired in via `override_path` in `main/idf_component.yml`. The component manager uses that copy and never re-downloads the BSP; **edit the display/bridge code there, not in `managed_components/`** (the latter is gitignored, unused, and re-fetched pristine). All other managed components (ft5x06 touch, hx8399, etc.) are still pulled normally from the registry and were not modified.
