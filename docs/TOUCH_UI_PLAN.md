# Pico FIDO Touch — ESP32-S3 + Waveshare 2" Capacitive Touch LCD

> **Hardware pivot (2026-07).** The target moved from the RP2040-based
> *Waveshare RP2040-Touch-LCD-1.69* to an **ESP32-S3 DevKit** driving the
> external [Waveshare 2" Capacitive Touch LCD](https://www.waveshare.com/wiki/2inch_Capacitive_Touch_LCD).
> This document is the ESP32-S3 plan. The earlier RP2040 scaffold (LVGL UI +
> drivers under `src/display/`) still builds and is kept as a reference; see
> [Legacy RP2040 scaffold](#legacy-rp2040-scaffold) for what carries over.

This describes how **pico-fido** is extended to use the touchscreen for three
things, unchanged from the original goals:

- **A. Approve/Deny prompt** for FIDO user-presence confirmation — an on-screen
  button that shows the relying party (and user, for registration) being
  approved, instead of a physical button.
- **B. Status display** — idle/connected state (battery is dropped: the 2" LCD
  module and a plain ESP32-S3 DevKit have no fuel gauge / charger).
- **C. Management UI** — read-only lists of resident FIDO credentials and OATH
  account names.

## Why the pivot helps: real secure key storage

The RP2040 has **no** secure key storage — external QSPI flash is unencrypted
and dumpable (see the main README → *Security Considerations*), which is why the
RP2040 build was explicitly a "prototype, accept the risk".

The **ESP32-S3 removes that limitation.** With **Flash Encryption** + **Secure
Boot v2** enabled, the master key encryption key (MKEK) lives in eFuse and is
inaccessible to external code, so all resident keys/seeds are encrypted at rest
and only signed firmware runs. pico-fido already advertises this
("Secure Boot and Secure Lock in RP2350 and ESP32-S3", README line 48). So on
this hardware the device can offer genuine hardware-backed protection — no
longer just a prototype caveat.

> ⚠️ Enabling Flash Encryption / Secure Boot **burns eFuses and is
> irreversible**. Do it deliberately (release mode, key management, `espefuse`)
> and only once the firmware is stable. It is a deployment/`sdkconfig` decision,
> not a code change — see *Next steps*.

## Target hardware

**Board:** ESP32-S3 DevKit (dual-core Xtensa LX7, native USB-OTG, PSRAM
depending on module). **Display module:** Waveshare 2" Capacitive Touch LCD.

| Function | Chip | Bus | Notes |
|----------|------|-----|-------|
| Display  | **ST7789T3** | 4-wire SPI | 240×320, IPS, 262K colours, command-compatible with the `esp_lcd` ST7789 driver |
| Touch    | **CST816D**  | I2C | CST81x family; works with the `esp_lcd_touch_cst816s` driver |
| microSD  | (on module)  | shared SPI | present but unused by this firmware |

Full-frame 240×320 RGB565 = 150 KB; the ESP32-S3 has ample RAM (and usually
PSRAM) so we are not forced into the tiny partial buffer the RP2040 needed.

### Reference wiring (Waveshare ESP32-S3 example)

The module connects over a 15-pin FPC/header. The ESP32-S3 uses a GPIO matrix,
so any free GPIOs work; the table below is Waveshare's own ESP32-S3 example and
is the recommended default. **Verify against the specific DevKit before
trusting it.**

| LCD pin  | ESP32-S3 GPIO | LCD pin | ESP32-S3 GPIO |
|----------|---------------|---------|---------------|
| 3V3      | 3V3           | LCD_CS  | GPIO39        |
| GND      | GND           | LCD_DC  | GPIO41        |
| MISO     | GPIO42        | LCD_RST | GPIO40        |
| MOSI     | GPIO2         | LCD_BL  | GPIO6         |
| SCLK     | GPIO1         | TP_SDA  | GPIO15        |
| SD_CS    | GPIO38        | TP_SCL  | GPIO7         |
| TP_INT   | GPIO17        | TP_RST  | GPIO16        |

These become Kconfig/`sdkconfig` values (or a small `board_config.h`), not a
Pico SDK board header.

## How it builds on ESP32-S3 (ESP-IDF)

pico-fido **already supports ESP32** via ESP-IDF — the top-level
`CMakeLists.txt` has an `ESP_PLATFORM` branch that registers `src/fido` and the
`pico-keys-sdk/config/esp32/components/*` components and includes
`$IDF_PATH/tools/cmake/project.cmake`. Build is:

```sh
idf.py set-target esp32s3
idf.py menuconfig      # enable OATH/OTP, display UI, pins; later: flash enc / secure boot
idf.py build flash monitor
```

The same SDK seams the RP2040 UI uses are **already wired on ESP32**:

- `picokey_init()` (WEAK) is called at boot on both platforms
  (`pico-keys-sdk/src/main.c`).
- `execute_tasks()` calls `platform_ui_task()` when `ENABLE_LVGL_UI` /
  `ENABLE_DISPLAY_UI` is defined — and on ESP32 that runs inside the
  `core0_loop` FreeRTOS task (`xTaskCreatePinnedToCore(core0_loop, ...)`).
- The signal bus (`SIGNAL_USER_PRESENCE_REQUEST/_COMPLETED/_CANCELLED/_TIMEOUT`,
  `SIGNAL_USB_MOUNTED`) is platform-independent.
- The FIDO→UI hooks added for the RP2040 are platform-independent C and carry
  over unchanged: `display_ui_set_context()` (Approve-screen context) and
  `fido_ui_list_credentials()` / `fido_ui_list_oath()` (management lists).

**What currently blocks the ESP32 UI:** the display integration is gated
`AND NOT ESP_PLATFORM` in `CMakeLists.txt` (the RP2040 path pulls in the Pico
SDK, the `lib/lvgl` git submodule, and bit-banged drivers). The port replaces
that path, not the UI logic.

## Display / touch / LVGL architecture on ESP-IDF

Use Espressif's managed components instead of the RP2040 submodule + hand-written
drivers (declare them in an `idf_component.yml`):

- **`esp_lcd`** (built into IDF): `esp_lcd_new_panel_io_spi()` +
  `esp_lcd_new_panel_st7789()` drive the ST7789T3 over the SPI bus.
- **`espressif/esp_lcd_touch_cst816s`**: CST816D touch over I2C.
- **`lvgl/lvgl` (v9)** + **`espressif/esp_lvgl_port`**: `esp_lvgl_port` creates
  the LVGL task, tick, and draw buffers, and binds an `esp_lcd` panel +
  `esp_lcd_touch` device to an LVGL display/indev — replacing the manual
  `disp_flush` / `touch_read` / `lv_tick_set_cb` callbacks and the `.S`-file
  filtering we needed on Cortex-M0+.

### Concurrency model (important difference from RP2040)

On the RP2040 the UI was single-threaded on core 0 (`platform_ui_task()` and the
signal handlers all ran there), so UI code touched LVGL with no lock. On ESP32-S3
under FreeRTOS, `esp_lvgl_port` runs LVGL in its **own task**, so any code that
mutates LVGL objects from another task (e.g. a signal handler dispatched on the
FIDO task) **must hold the port lock** (`lvgl_port_lock()` / `lvgl_port_unlock()`).

The copy-only design already in place makes this clean:
`display_ui_set_context()` still only copies strings (safe from any task); the
screen switch / label updates move behind the LVGL lock. Decide during
implementation whether to (a) keep the existing `platform_ui_task()` pump and
take the lock in the signal handlers, or (b) fully adopt `esp_lvgl_port`'s task
and drop the manual pump. (a) is the smaller diff; (b) is more idiomatic.

## Feature mapping (A / B / C)

| Feature | RP2040 today | ESP32-S3 plan |
|---------|--------------|---------------|
| A Approve/Deny | LVGL confirm screen + `touch_accept_button`/`cancel_button`; RP/user via `display_ui_set_context()` | **Same UI/logic**; buttons/context unchanged. Confirm the ESP32 `button_wait()` path emits the presence signals and honours `touch_accept_button`. |
| B Status | connection + **battery** | connection only (no battery HW); optional: show serial/label |
| C Management | credential + OATH lists via `fido_ui_list_*()` | **Same**, unchanged |

## The SDK "approve" hook

The RP2040 fork wired the SDK's reserved `touch_accept_button` into
`button_wait()` so an on-screen tap counts as a press. Confirm this same change
is present/needed on the ESP32 `button_wait()` path in
`MildlyTyped/pico-keys-sdk` (branch `touch-screen-support`); it is
platform-independent C in `pico-keys-sdk/src/button.c` and should apply as-is.

## Legacy RP2040 scaffold

Kept in the repo as reference; **carries over unchanged** to ESP32-S3:

- `src/display/display_ui.c` screen state machine, LVGL screen builders, signal
  handlers, and the `refresh_list()` management lists — only the LVGL *binding*
  (flush/input/tick) and locking change.
- `src/display/lv_conf.h` — reusable (may instead be provided via `esp_lvgl_port`
  Kconfig; RGB565 stays).
- FIDO-side hooks: `display_ui_set_context()`, `fido_ui_list_credentials()`,
  `fido_ui_list_oath()` (in `fido.c` / `credential.c` / `oath.c`).

**Replaced on ESP32-S3:**

- `src/display/st7789.c`, `cst816.c`, `battery.c` — RP2040 SPI/I2C/ADC drivers →
  `esp_lcd` + `esp_lcd_touch_cst816s` (battery dropped).
- `src/boards/waveshare_rp2040_touch_lcd_1_69.h` and the Pico-SDK board-header
  install in `CMakeLists.txt` — not applicable on ESP-IDF.
- `lib/lvgl` git submodule + `.S` filtering → `lvgl/lvgl` managed component.

## Next steps

1. **Branch/naming.** New work on a branch off `main` (e.g.
   `devin/esp32s3-touch`). Decide whether to rename the repo focus from
   `fido-touch` (RP2040) — kept as-is for now.
2. **Bring up the ESP-IDF UI path.** In `CMakeLists.txt`, add an ESP32 display
   path (register the display sources + `ENABLE_DISPLAY_UI`/`ENABLE_LVGL_UI` for
   ESP-IDF) parallel to the RP2040 one; add `idf_component.yml` pulling
   `lvgl/lvgl` (^9), `espressif/esp_lvgl_port`, `espressif/esp_lcd_touch_cst816s`.
3. **Panel + touch init.** New `display_esp32.c` (or refactor `display_ui.c`'s
   init): SPI bus, `esp_lcd` ST7789 panel, CST816 touch, `esp_lvgl_port`
   display/indev. Move pins to Kconfig using the wiring table above.
4. **Wire LVGL locking** into the signal handlers (or adopt the port task);
   keep `display_ui_set_context()` copy-only.
5. **Verify presence flow** end-to-end on ESP32: `button_wait()` emits the
   signals and honours `touch_accept_button`/`cancel_button`.
6. **Confirm pins & touch orientation** on the actual DevKit (rotation/mirror in
   `esp_lcd`/LVGL); the 2" panel is 240×320 portrait.
7. **Security hardening (deployment).** Once stable, enable Flash Encryption +
   Secure Boot v2 in `sdkconfig` (release mode) and document eFuse burning.
   Update the README/security note to reflect that this build *can* be
   hardware-secured (unlike the RP2040 prototype).
8. **Docs.** Keep this plan and the README fork note in sync as the port lands.

## Open questions

- Which exact ESP32-S3 DevKit (module/flash/PSRAM, USB-OTG vs UART bridge)? It
  affects USB (native TinyUSB is required for HID/CCID) and available GPIOs.
- Are the Waveshare reference GPIOs acceptable, or is there a preferred pinout?
- Ship with Flash Encryption / Secure Boot from the start, or add after bring-up?
- Keep the RP2040 scaffold in-tree as a second target, or remove it once the
  ESP32-S3 path works?
