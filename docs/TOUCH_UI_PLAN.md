# Pico FIDO Touch — Waveshare RP2040-Touch-LCD-1.69 support

This document describes how **pico-fido** is extended to run on the
[Waveshare RP2040-Touch-LCD-1.69](https://www.waveshare.com/wiki/RP2040-Touch-LCD-1.69),
a plain RP2040 board with a 1.69" 240×280 IPS LCD, capacitive touch, an IMU
and a Li-ion charger. The goal is to use the touchscreen for three things:

- **A. Approve/Deny prompt** for FIDO user-presence confirmation — replacing the
  BOOTSEL button tap with an on-screen button that shows what is being approved.
- **B. Status display** — idle/connected state and battery level.
- **C. Management UI** — a menu skeleton for browsing credentials and OATH codes.

The code in this repository is a **scaffold**: it builds into a flashable
`pico_fido.uf2`, brings up the panel, reads touch input and drives the three
screens, but several items (listed under *Follow-ups*) still need real hardware
bring-up and deeper FIDO integration.

## Target hardware

| Function        | Chip       | Bus            | Pins (GP)                                   |
|-----------------|------------|----------------|---------------------------------------------|
| Display         | ST7789V2   | SPI1 @ 40 MHz  | DC 8, CS 9, CLK 10, MOSI 11, RST 13, BL 25  |
| Touch           | CST816T    | I2C1 @ 400 kHz | SDA 6, SCL 7, INT 21, RST 22 (addr 0x15)    |
| IMU (optional)  | QMI8658    | I2C1 (shared)  | SDA 6, SCL 7 (addr 0x6B)                     |
| Battery sense   | —          | ADC            | GP29 / ADC3 via divider (verify!)           |

Panel geometry: 240×280 visible out of the ST7789 240×320 GRAM, so rows are
offset by 20 (`LCD_ROW_OFFSET`). Pin values come from the Waveshare vendor C
demo (`lib/Config/DEV_Config.h`, `lib/LCD/*`) and are centralised in
[`src/display/board_config.h`](../src/display/board_config.h). **They must be
verified against the board schematic before trusting them on hardware.**

## How it plugs into pico-fido

pico-fido already has almost everything needed; the firmware is built on the
`pico-keys-sdk` submodule which exposes the right seams:

- **`picokey_init()`** — a `WEAK` hook the SDK calls once at boot. We override it
  (in `display_ui.c`) to initialise the display, touch and battery and to
  register signal handlers.
- **`platform_ui_task()`** — the SDK's core-0 main loop (`execute_tasks()`) calls
  this every iteration when `ENABLE_DISPLAY_UI` (or `ENABLE_LVGL_UI`) is defined.
  We use it to poll touch and redraw.
- **The signal bus** (`signal.h`) — `button_wait()` already emits
  `SIGNAL_USER_PRESENCE_REQUEST / _COMPLETED / _CANCELLED / _TIMEOUT`, and
  `usb_task()` emits `SIGNAL_USB_MOUNTED`. The UI subscribes to these to switch
  screens; nothing in the FIDO core has to change.

### The one required SDK change

User-presence confirmation funnels through `button_wait()` in
`pico-keys-sdk/src/button.c` (running on core 0, where keep-alive, LED and
timeout handling live). It confirms on a physical button read and cancels on the
`cancel_button` flag, but there was **no way to inject an "approve" from
software** — even though the SDK already declared a `touch_accept_button` flag
for exactly this and left it unwired.

The fork wires it in (2 lines): `button_wait()` now treats `touch_accept_button`
as a press, and the `platform_ui_task()` hook is enabled by `ENABLE_DISPLAY_UI`
too. This lives on the `touch-screen-support` branch of the
`MildlyTyped/pico-keys-sdk` fork, which the submodule points at. Keeping the
change in the SDK means keep-alive to the host, the timeout and the LED status
all keep working unchanged while the user takes time to tap.

```
    while (button_pressed == false && cancel_button == false) {
        execute_tasks();                 // -> platform_ui_task() polls touch
        ...
        button_pressed = picok_board_button_read() || touch_accept_button;
    }
```

The UI's approve handler sets `touch_accept_button = true`; the deny handler sets
`cancel_button = true`. Because both the handler and `button_wait()` run on
core 0, there is no cross-core race.

## Module layout (`src/display/`)

| File            | Responsibility                                                    |
|-----------------|-------------------------------------------------------------------|
| `board_config.h`| All pin / bus / geometry constants for the board.                 |
| `st7789.c/.h`   | ST7789V2 SPI driver: init, fill-rect, blit, backlight.            |
| `cst816.c/.h`   | CST816T I2C touch driver: init + `cst816_read()` (point + gesture)|
| `battery.c/.h`  | ADC battery voltage → percent.                                    |
| `display_ui.c/.h`| LVGL screens, signal handlers, `platform_ui_task/init`.          |
| `lv_conf.h`     | Minimal LVGL config (RGB565, 32 KB pool, Montserrat fonts).       |
| `../boards/waveshare_rp2040_touch_lcd_1_69.h` | Pico SDK board header (16 MB flash). |
| `../../lib/lvgl` | LVGL v9.2.2 submodule (UI toolkit).                              |

The UI is built with **LVGL** (v9.2.2, vendored as the `lib/lvgl` submodule).
`display_ui.c` provides the two LVGL platform callbacks — a display *flush* that
pushes rendered tiles through `st7789_blit()`, and a pointer *input* read that
wraps `cst816_read()` — plus a runtime tick from `board_millis()`. Rendering is
**partial** (a 240×40 RGB565 draw buffer, ~19 KB) to fit RP2040 SRAM, and the
Helium/Neon assembly accelerators are excluded from the LVGL build (the C
blenders are used instead). Screens are plain LVGL objects switched with
`lv_screen_load()`; `lv_timer_handler()` is pumped from `platform_ui_task()`.

## Screens

- **B — Status (default/idle).** "PICO FIDO", connection state (Ready /
  Connected, driven by `SIGNAL_USB_MOUNTED`) and battery %. Tap → Menu.
- **A — Confirm.** Shown on `SIGNAL_USER_PRESENCE_REQUEST`. Shows the relying
  party (and user name for make-credential), a live countdown, and
  full-width **APPROVE** (green) / **DENY** (red) buttons. Approve →
  `touch_accept_button`; Deny → `cancel_button`. Auto-returns to Status on
  complet/cancel/timeout.
- **C — Menu.** Credentials / OATH codes / Device info / Back. The sub-screens
  are placeholders today (see below).

## Build & flash

```sh
git clone --recurse-submodules https://github.com/MildlyTyped/fido-touch
cd fido-touch
mkdir build && cd build
PICO_SDK_PATH=/path/to/pico-sdk cmake .. \
    -DPICO_BOARD=waveshare_rp2040_touch_lcd_1_69 \
    -DENABLE_DISPLAY_UI=1
make -j4
```

Selecting `-DPICO_BOARD=waveshare_rp2040_touch_lcd_1_69` installs the bundled
board header into the Pico SDK's board dir at configure time (the pico-keys SDK
reads it directly from there), so no `-DPICO_BOARD_HEADER_DIRS` is needed.
`-DPICO_BOARD=pico` also works (2 MB flash assumed). Enabling the UI defines
both `ENABLE_DISPLAY_UI` and `ENABLE_LVGL_UI` (the SDK's reserved task hook) and
links LVGL. Copy `pico_fido.uf2` to the board in BOOTSEL mode.
`ENABLE_DISPLAY_UI` defaults to **OFF**, so non-display builds are unaffected.

## Follow-ups (scaffolded but not finished)

1. **Verify pins & touch mapping** against the schematic; add touch rotation /
   calibration if X/Y are swapped or inverted.
2. **Relying-party context** — *done.* `cbor_make_credential` /
   `cbor_get_assertion` call `display_ui_set_context()` (a weak no-op in
   `fido.c`, overridden by the display build) before requesting presence, so
   screen A shows the RP id (+ user name for make-credential). The FIDO handlers
   run on core 1, so only the strings are copied there; the LVGL labels are
   updated from the core-0 presence-request handler.
3. **Credentials & OATH screens (C)** — enumerate resident credentials and
   render live TOTP codes. Needs read-only accessors into the FIDO/OATH stores.
4. **PWM backlight** dimming (vendor demo uses PWM on GP25) and screen
   blanking / low-power sleep when idle to save battery.
5. **IMU (QMI8658)** — optional orientation / tap-to-wake.
6. **Richer LVGL UI** — the toolkit is in place; add themes, animations, a
   scrollable credential list, and QR/large fonts as needed.
7. **Security note** — the RP2040 has no secure key storage (see the main
   README). A display does not change that; do not present it as a hardware
   security module.
8. **Strict warnings** — add `src/display/` to `picokeys_apply_strict_flags`
   once the drivers are hardware-validated.

## Testing

- **Host build check:** the display/LVGL sources are excluded from the
  emulation target (`-DENABLE_EMULATION=1`) by CMake, and the driver hardware
  code is additionally guarded behind `#ifndef ENABLE_EMULATION`, so this change
  does not affect it. (The emulation binary separately needs the `tss2` TPM
  headers, unrelated to the display work.)
- **Board build check:** `-DPICO_BOARD=waveshare_rp2040_touch_lcd_1_69
  -DENABLE_DISPLAY_UI=1` produces a valid `pico_fido.uf2` with LVGL linked
  (verified in this branch; ~126 KB SRAM/BSS used).
- **On hardware (to do):** confirm panel init/colours, touch coordinates,
  approve/deny flow end-to-end against a WebAuthn test page, and battery
  reading.
