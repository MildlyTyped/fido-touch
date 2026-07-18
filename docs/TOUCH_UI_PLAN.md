# Pico FIDO Touch — ESP32-S3 + Waveshare 2" Capacitive Touch LCD

> **Hardware pivot (2026-07).** The target moved from the RP2040-based
> *Waveshare RP2040-Touch-LCD-1.69* to the **ESP32-S3-N16R8-EXT** DevKit
> (16 MB flash, 8 MB octal PSRAM, native USB-OTG) driving the external
> [Waveshare 2" Capacitive Touch LCD](https://www.waveshare.com/wiki/2inch_Capacitive_Touch_LCD).
>
> **Status: implemented.** The touch UI now builds on ESP-IDF v5.5 for the
> ESP32-S3 using `esp_lcd` + `esp_lcd_touch_cst816s` + `esp_lvgl_port`, and the
> RP2040 touchscreen scaffold has been **removed** (base RP2040 FIDO firmware is
> unaffected). Not yet validated on physical hardware — pins, orientation, and
> the end-to-end WebAuthn ceremony still need on-board bring-up.

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
> irreversible**. The build ships the config ready to go (`sdkconfig.defaults.secure`)
> but it is **opt-in** and never applied by default — see
> [Hardware security provisioning](#hardware-security-provisioning). Nothing in
> ordinary `idf.py build` touches eFuses.

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

These pins are defined as `#define`s at the top of `src/display/display_ui.c`
(MISO is unused — the panel is write-only), not a Pico SDK board header.

## How it builds on ESP32-S3 (ESP-IDF)

pico-fido **already supports ESP32** via ESP-IDF — the top-level
`CMakeLists.txt` has an `ESP_PLATFORM` branch that registers `src/fido` and the
`pico-keys-sdk/config/esp32/components/*` components and includes
`$IDF_PATH/tools/cmake/project.cmake`. The touch UI is enabled with
`-DENABLE_DISPLAY_UI=1`, which adds `src/display` as an ESP-IDF component (its
`idf_component.yml` pulls `lvgl` 9.3, `esp_lvgl_port`, `esp_lcd_touch_cst816s`)
and applies the `sdkconfig.defaults.display` fragment (LVGL fonts, RGB565).

> **Toolchain, in plain terms.** `esptool` only *flashes* a prebuilt binary; it
> cannot compile firmware. **Compiling requires ESP-IDF**, which bundles
> `esptool.py`, `espefuse.py`, and the serial monitor. So the flow is: install
> ESP-IDF once, build, then flash (with `idf.py flash` *or* bare `esptool.py`).

See [macOS: setup → build → flash → bring-up](#macos-setup--build--flash--bring-up)
for the full walkthrough.

## macOS: setup → build → flash → bring-up

Tested layout: ESP-IDF **v5.5** installed at `~/esp/esp-idf`. The
ESP32-S3-N16R8-EXT enumerates over **native USB** (USB-OTG), so on macOS it
appears as a `/dev/cu.usbmodem*` port with no extra driver.

### 1. Install prerequisites (Homebrew)

```sh
# Homebrew (skip if already installed): https://brew.sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# ESP-IDF's own prerequisites on macOS
brew install cmake ninja dfu-util python git
```

### 2. Install ESP-IDF v5.5 + the ESP32-S3 toolchain

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32s3        # downloads the xtensa-esp32s3 compiler + tools
```

Load the environment in **every new terminal** before building/flashing (this is
what puts `idf.py`, `esptool.py`, `espefuse.py` on your `PATH`):

```sh
. ~/esp/esp-idf/export.sh
```

### 3. Get the code (with submodules) and build

```sh
git clone --recursive https://github.com/MildlyTyped/fido-touch.git
cd fido-touch
# if you cloned without --recursive:
git submodule update --init --recursive

. ~/esp/esp-idf/export.sh
idf.py -B build-esp -DENABLE_DISPLAY_UI=1 set-target esp32s3
idf.py -B build-esp build          # -> build-esp/pico_fido.bin
```

A plain `idf.py set-target esp32s3 && idf.py build` (no `ENABLE_DISPLAY_UI`)
produces the headless FIDO firmware instead.

### 4. Find the serial port

Put the board in download mode if needed (hold **BOOT**, tap **RESET**, release
**BOOT**), then:

```sh
ls /dev/cu.usbmodem*       # e.g. /dev/cu.usbmodem101
```

### 5. Flash

Easiest (ESP-IDF wrapper):

```sh
idf.py -B build-esp -p /dev/cu.usbmodem101 flash monitor
```

Or with **bare esptool** (no `idf.py`) — exact offsets come from
`build-esp/flasher_args.json`; the standard (non-secure) build is:

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0     build-esp/bootloader/bootloader.bin \
  0x8000  build-esp/partition_table/partition-table.bin \
  0x20000 build-esp/pico_fido.bin
```

### 6. Bring-up / monitor

```sh
idf.py -B build-esp -p /dev/cu.usbmodem101 monitor   # Ctrl-] to quit
# or any serial terminal at 115200 baud:
#   screen /dev/cu.usbmodem101 115200
```

First-boot checklist on the actual board:

1. Serial log shows the panel + touch init with no `ESP_ERROR_CHECK` aborts.
2. Backlight (GPIO6) on; the status screen renders (fix colours with
   `esp_lcd_panel_invert_color`, orientation with the panel/`esp_lcd_touch`
   mirror/swap flags in `src/display/display_ui.c`).
3. A touch registers at the right coordinates (adjust `swap_xy`/`mirror_*`).
4. The host sees a FIDO authenticator over USB; a WebAuthn registration shows
   the Approve/Deny screen with the relying party, and a tap approves.

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
`display_ui_set_context()` still only copies strings (safe from any task). The
implementation uses **`esp_lvgl_port`'s own LVGL task** for rendering/timers, and
every signal handler and `platform_ui_task()` update takes the port lock
(`lvgl_port_lock()`/`lvgl_port_unlock()`) around any LVGL object access.

## Feature mapping (A / B / C)

| Feature | Original RP2040 | ESP32-S3 |
|---------|-----------------|----------|
| A Approve/Deny | LVGL confirm screen + `touch_accept_button`/`cancel_button`; RP/user via `display_ui_set_context()` | **Implemented**, same UI/logic; buttons/context unchanged. |
| B Status | connection + **battery** | connection only (battery dropped) |
| C Management | credential + OATH lists via `fido_ui_list_*()` | **Implemented**, unchanged |

## The SDK "approve" hook

The fork wired the SDK's reserved `touch_accept_button` into `button_wait()` so
an on-screen tap counts as a press. This is platform-independent C in
`pico-keys-sdk/src/button.c` (branch `touch-screen-support`) and applies
unchanged on ESP32. On-hardware verification that the ESP32 `button_wait()` path
emits the presence signals and honours `touch_accept_button`/`cancel_button` is
still pending a physical board.

## What carried over vs. what was removed

**Carried over unchanged** (platform-independent):

- `src/display/display_ui.c` screen state machine, LVGL screen builders, signal
  handlers, and the management lists — only the LVGL *binding* (panel/touch
  init + locking) is ESP-IDF-specific now.
- FIDO-side hooks: `display_ui_set_context()`, `fido_ui_list_credentials()`,
  `fido_ui_list_oath()` (in `fido.c` / `credential.c` / `oath.c`).

**Removed with the RP2040 pivot:**

- `src/display/st7789.c`, `cst816.c`, `battery.c` — RP2040 SPI/I2C/ADC drivers →
  `esp_lcd` + `esp_lcd_touch_cst816s` (battery dropped).
- `src/display/lv_conf.h` → LVGL configured via `esp_lvgl_port` Kconfig +
  `sdkconfig.defaults.display`.
- `src/boards/waveshare_rp2040_touch_lcd_1_69.h` and the Pico-SDK board-header
  install in `CMakeLists.txt` — not applicable on ESP-IDF.
- `lib/lvgl` git submodule + `.S` filtering → `lvgl/lvgl` managed component.

## Hardware security provisioning

The firmware ships **ready** for Flash Encryption + Secure Boot v2, but never
enables them automatically — burning eFuses is irreversible and must be a
deliberate provisioning step on the real board. The config lives in
`sdkconfig.defaults.secure` (opt-in) and moves the partition table to `0x10000`
(the signed bootloader is larger than the default `0x8000` offset).

```sh
. ~/esp/esp-idf/export.sh

# 1. Generate an RSA-3072 Secure Boot v2 signing key. Keep it OFFLINE and out of
#    git (.gitignore already excludes *.pem). Losing it means no more updates.
espsecure.py generate_signing_key --version 2 secure_boot_signing_key.pem

# 2. Configure + build with the secure fragment appended.
idf.py -B build-secure \
  -DENABLE_DISPLAY_UI=1 \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.display;sdkconfig.defaults.secure" \
  set-target esp32s3
idf.py -B build-secure build

# 3. FIRST FLASH BURNS eFUSES (Secure Boot key digest + Flash Encryption key)
#    and encrypts flash in place. Irreversible. Do it once, on the target board.
#    idf.py sequences the irreversible steps safely; prefer it over bare esptool
#    for the secure flow (the offsets/encryption differ from the normal build -
#    see build-secure/flasher_args.json).
idf.py -B build-secure -p /dev/cu.usbmodem101 flash monitor

# 4. Inspect what was burned (read-only):
espefuse.py -p /dev/cu.usbmodem101 summary
```

After provisioning, resident keys/seeds are encrypted at rest with a key held in
eFuse, and only firmware signed with your key boots. Until this is actually run
on hardware, **do not claim the device is hardware-secured** — the default build
is functionally identical to an unprotected one.

## Remaining work (needs physical hardware)

Code is complete and all targets build; the following need the actual board:

1. **Confirm pins & touch orientation** on the DevKit (rotation/mirror in the
   `esp_lcd_touch_config_t.flags` / panel config); the 2" panel is 240×320.
2. **Verify colour/inversion** (`esp_lcd_panel_invert_color`) and backlight on
   GPIO6.
3. **Verify the presence flow** end-to-end: `button_wait()` emits the signals
   and honours `touch_accept_button`/`cancel_button` during a real WebAuthn
   ceremony.
4. **Provision security** per the section above once bring-up is stable.

## Decisions (previously open questions)

- **Board:** ESP32-S3-N16R8-EXT (16 MB flash, 8 MB octal PSRAM, native USB-OTG).
- **Pinout:** Waveshare reference GPIOs (table above).
- **Security:** enabled from the start as an opt-in, ready-to-provision config
  (`sdkconfig.defaults.secure`); eFuses are only burned during provisioning.
- **RP2040 scaffold:** removed now that the ESP32-S3 target builds.
