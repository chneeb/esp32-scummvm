# Plan: ScummVM as an app for the ESP32-P4 Launcher

Status: plan only, nothing implemented. Written 2026-11-11.

Today this fork is a standalone ESP-IDF firmware: it owns the whole board (display bridge, touch, codec, SD card,
USB host) and is flashed in place of everything else. The goal is a **`scummvm.papp`** that the
[ESP32-P4 Launcher](https://github.com/giltal/ESP32_P4_Launcher) (local checkout: `~/Source/ESP32_P4_Launcher`)
loads from the SD card like its emulators, so ScummVM runs next to the other apps and on every board the launcher
supports (Guition 4.3"/7", M5Stack Tab5, Waveshare 3.5", this Waveshare dev kit + 5" panel).

The standalone `backends/platform/esp32/` stays as it is. The launcher version is a second backend beside it.

## How a launcher app differs

A `.papp` is a relocatable image loaded into PSRAM and run from there. It never touches hardware or ESP-IDF; it talks to
the launcher through versioned interfaces (`papp-sdk/include/papp/papp.h`, `components/papp_abi/include/papp_abi.h`):

| Interface | What ScummVM would use |
|---|---|
| SYS / MEM | `papp_time_us`, `papp_delay_ms`, logging, `papp_alloc` (malloc goes there through the SDK's newlib glue) |
| THRD | threads (audio), recursive mutexes |
| FS | files on the card (`/sd/...`); newlib's `fopen`/`open`/`stat` sit on it |
| DISP | an RGB565 surface of any size, scaled to the panel by the launcher (PPA), touch in surface coordinates, `papp_disp_info` |
| AUDI | a stream at any rate (resampled and mixed by the launcher) |
| INPT | gamepads (USB, XInput, board pad), USB keyboard events with HID usage + text |
| UI | launcher-drawn file picker, menus, message box |

The launcher already brings up the display, touch, codec, SD card, USB host and Wi-Fi. HOME (or holding Select+Start)
opens its pause menu (Resume, Volume, Screenshot, Quit to home) over any app.

C++ apps exist already (Lynx, Atari 2600, Atari 800): `-lstdc++ -lsupc++`, static constructors via `.init_array`,
`apps/common/odroid/cxx_papp.c` maps libstdc++'s pthread calls onto THRD mutexes.

## What changes, part by part

| Standalone fork (`backends/platform/esp32/`) | Launcher backend (`backends/platform/papp/`, new) |
|---|---|
| `app_main`, 512 KB main task, USB HID task | the app's main thread: `PAPP_MANIFEST(.main_stack = 512 KB, .flags = PAPP_MF_STACK_PSRAM)`, then `scummvm_main()` |
| `bsp_override/` (TC358762, ATTINY88, DPI panel, RGB888, B,G,R order) | **dropped**: the launcher's board package does this for every board |
| `esp-graphics.cpp`: CLUT8 → RGB565 → PPA → RGB888 panel buffer, gfx task | CLUT8 → RGB565 into a launcher surface the size of the game screen (320x200, 640x480, ...); the launcher scales it. Overlay (GUI) = a second surface the size of the panel (`papp_disp_info`: 800x480 here, 480x320 on the 3.5", 1280x720 on Tab5). Switch surfaces when ScummVM toggles the overlay. Aspect correction (320x200 shown as 4:3): STRETCH into a 4:3 surface or scale in software. Software mouse cursor (the fork has none; needed for USB mouse / pad-as-mouse) |
| FT5x06 touch driver | `papp_disp_touch` (already in surface coordinates); same tap → mouse down/up logic, two fingers → virtual keyboard |
| `esp-mixer.cpp`: codec via BSP, audio task | `papp_audio_open` at the mixer rate (e.g. 22050 or 44100, stereo S16); an audio thread calls `mixCallback` and writes the stream |
| `esp-mutex.cpp` (FreeRTOS recursive mutex) | `papp_mutex_create(true)` |
| `getMillis` / `delayMillis` via `esp_timer` / `vTaskDelay` | `papp_time_us` / `papp_delay_ms`; DefaultTimerManager stays, ticked from `pollEvent` as now |
| `usb_hid.c` + `hid_ev` (own USB host stack) | INPT events: `PAPP_EV_KEY_DOWN/UP` carry the HID usage and the character, so the fork's `hid_keys.h` mapping is reused. USB host is the launcher's (works on the lower USB-A port of this kit too) |
| CardKB on I2C (0x5F) | dropped at first. Later via a launcher I2C-input feature (the NES/SNES Classic pad driver on `waveshare-classic-pad` is the start of that) |
| `mmc.c` + `blkcache` (SD mount, read cache) | **dropped**: the launcher owns the card. Possibly a read cache in the app if engines read in small pieces (measure first) |
| `posixesp-fs-factory.cpp` (root `/sdcard/`) | POSIX FS node on `/sd/` |
| config/data under `/sdcard/scummvm/` | `/sd/scummvm/` (scummvm.ini, themes, engine data, saves); games anywhere on the card |
| `quit()` reboots / loops | return from `scummvm_main`; `papp_exit_requested()` (launcher "Quit to home") → `EVENT_QUIT` |
| gamepads: none | new: pad → mouse (stick / D-pad moves the cursor, A = left click, B = right click), Start = ScummVM menu (F5), Select = skip/Escape; ScummVM's keymapper can take the rest |

## Blockers on the launcher side (need Gil)

1. **FS interface has no directory listing and no rename.** `papp_if_fs_t` v1 has open/read/write/seek/tell/size/remove/mkdir
   only, and the libc glue returns ENOSYS for `rename` and has no `opendir`/`readdir`. ScummVM needs both: game detection,
   "Add game" / mass add, the save list (`listSavefiles`) and theme lookup enumerate directories; saves are written to a
   temporary file and renamed. Proposal: FS v2 appended to the interface (`dir_open` / `dir_read` (name, is_dir, size) /
   `dir_close`, `rename`, `stat` that knows directories) plus `opendir`/`readdir`/`rename` in `libc_glue.c`. Small and
   useful to other apps (file managers, any port with its own browser). This is the one hard requirement.
2. **Distribution size.** The Store takes a community `.papp` up to 4 MB in the repository; above that `app.json` gives a
   `url` + `sha256` + `size` (e.g. a release asset on this fork). Already supported, nothing to change.
3. Optional: **USB mouse** in INPT (the launcher has no mouse events today). Touch + pad cover it without.

Limits that are fine as they are: image up to 24 MB (`MAX_IMAGE_SIZE` in `lx_app.c`), relocation offsets up to 128 MB,
the executable PSRAM alias is mapped per app.

## Build

ScummVM's own `configure` + `make libs` stays (as the ESP-IDF build uses it now). What changes:

- `configure`: a new host `papp` (beside `esp32`): `_backend=papp`, static plugins, no dynamic modules, same feature
  switches as the `esp32` host (no MT-32, no scalers, vkeybd on), `-fno-exceptions`. **Keep RTTI**: `common/` uses
  `dynamic_cast`. Compiler flags = the SDK's (`papp_build.py`: target arch/ABI, `-fno-common`, ...); no ESP-IDF include paths.
- zlib, libpng, libogg come from ESP-IDF managed components today. For the app they are built from source into the
  image (vendored, or as extra sources of the build script). Vorbis/MAD stay off at first, as now.
- `backends/platform/papp/build.py`: runs configure + `make libs`, compiles the backend, links everything with
  `papp-sdk/ld/papp.ld`, `--emit-relocs`, newlib + the SDK's libc glue, `-lstdc++ -lsupc++` and `cxx_papp.c`, then
  `papp_pack.py` → `build/scummvm.papp`. It finds the SDK through `PAPP_SDK=~/Source/ESP32_P4_Launcher/papp-sdk`.
- Engines: start with the fork's set (`groovie scumm agi sky sci queen parallaction sword1 sword2 dreamweb`, sub-engines
  `scumm-7-8 he sci32`), one switch list in `build.py`.
- Deploy while developing: `python tools/papp_dev.py --port /dev/ttyACM1 deploy build/scummvm.papp` (from the launcher
  checkout); `crash --elf build/scummvm.elf` decodes a crash.

## Memory and size (estimates, to measure in phase 1)

- The standalone binary is ~10.5 MB including ~1 MB of ESP-IDF, so the app image would be ~9-10 MB code + data, plus .bss.
- Relocations: 8 bytes each; C++ vtables and pointer tables make many absolute ones. Guess 200k-500k → 2-4 MB in the
  file and a few hundred ms to apply at load (Doom: 4,048 relocs in 33 ms).
- PSRAM: the launcher leaves ~28 MB free (32 MB on the chip). Minus the image → **~17 MB of heap** for ScummVM, against
  ~25-30 MB standalone. Most 2D games are far below that; SCI32 / Groovie video games and anything near the limit
  standalone (Broken Sword 2.5 already fails) may not fit. Engines can be dropped to save image size.
- Code runs from PSRAM through the cache (standalone: flash XIP through the same cache), so speed should be similar.
  Hot loops can go to internal SRAM with `PAPP_FAST` if needed.
- Loading ~10 MB from the card: about 2 s (Doom reads 0.6 MB in 112 ms).

## Phases

1. **Feasibility build** (no launcher change): `configure --host=papp`, a stub backend (null graphics/audio, logs only),
   link and pack. Answers: does it link against the SDK, image size, relocation count, load time, free heap at start.
   Run with `deploy`, expect the "no games" state or a log line.
2. **FS v2 in the launcher** (directory listing, rename, stat for directories): implement on its own branch of the
   launcher, test with a small test app, offer it to Gil as a PR first. Until merged, run it on the local `p4-test` build.
3. **Backend**: graphics (game surface + overlay surface, palette conversion, cursor), touch, keyboard events, audio
   thread, mutex/time, POSIX FS on `/sd/`, exit handling. Goal: launcher → ScummVM GUI → add a game → play with sound.
4. **Input and screens**: pad → mouse, keymapper defaults, virtual keyboard by two-finger tap; check the GUI on the 3.5"
   (480x320, ScummVM's low-res theme) and Tab5 (portrait panel, rotated by the launcher).
5. **Distribution**: `make papp-dist` for the `/sd/scummvm/` data (themes, engine data); icon; community entry in the
   launcher (`community/scummvm/`: icon + `app.json` with `url`/`sha256`/`size` pointing at a release of this fork).
   README for users: where the data goes, which games were tested.

## Open questions

- One `.papp` with all engines, or several (e.g. "ScummVM SCUMM", "ScummVM SCI") to keep each image and heap smaller?
- Does the launcher's FAT access keep up with engines that seek a lot (the fork added `blkcache` for that)? Measure in phase 3.
- Upstream ScummVM may later want a "papp" backend; keep it self-contained under `backends/platform/papp/`.
