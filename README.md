# Acer Predator Helios Neo 16 (PHN16-71): Mode-button LED

Reverse-engineering notes and a tool for the Mode-button LED on the PHN16-71
(Quanta "Compass_RTX" board, BIOS V1.20, ITE **IT5570E** EC, chip ID `0x5570`).
The same tool runs on Linux and Windows.

What you can do with it: read the current thermal mode from the EC, switch modes,
give the Mode button any of its 128 preset colours per mode (`--led-daemon`), switch
the button between static, breathing and colour cycle without a flash (`--led-effect`),
and set any RGB colour on the 4-zone keyboard (`--kbd-rgb`). A custom RGB colour on the
Mode button, or a colour change without the preset flash, has **not** been found; section 2
lists everything that was tried.

Everything below was measured on the live machine: first on Linux 6.17 (`acer_wmi`
loaded), then on Windows 11. An earlier version of these notes and tool got several
things wrong; section 4 lists the corrections.

> **Use at your own risk.** The tool reads EC memory and writes to the EC's SMBus
> host registers to talk to the lighting controller. Its write path is limited to
> those registers and every command was tested on one PHN16-71, but nothing here is
> supported by Acer. Other models or BIOS versions may differ.

| Path | What it is |
|---|---|
| `predator_i2ec.c`, `Makefile` | the tool (Linux: `make`, Windows: `mingw32-make`) |
| `windows/PredatorLights.ps1` | WMI-only alternative for mode and keyboard lighting |
| `windows/*_probe.ps1`, `page_sweep.ps1`, `palette_scan.ps1` | the Windows experiments behind section 2 (they write their logs next to themselves) |
| `windows/kbd_restore.ps1` | puts the keyboard effect back after the method-20 device test |
| `acer_gkbd/` | small Linux debugfs module for calling the BIOS WMI methods |

---

## 1. Where the current mode is stored

The ACPI EC address space (what the kernel reads through ports 0x62/0x66) is
mirrored at EC XRAM `0x0500–0x05FF`, so ACPI EC offset `xx` = XRAM `0x05xx`.

| Location | Meaning | Eco | Quiet | Balanced | Performance | Turbo |
|---|---|---|---|---|---|---|
| **ACPI EC `0x54`** = XRAM `0x0554` | current thermal mode (acer-wmi reads this) | `04` | `01` | `00` | `02` | `03` |
| XRAM `0x016F` | EC firmware's internal copy | `04` | `01` | `00` | `02` | `03` |
| XRAM `0x0AF4` | last WMI `SetGamingMiscSetting(0x0B, …)` value | `06` | `00` | `01` | `04` | `05` |
| SMBus-D `HOBDB` (`0x1C3B`) | last code sent to the LED controller | `01` | `02` | `03` | `04` | `05` |
| `platform_profile` (sysfs) | kernel name | `low-power` | `quiet` | `balanced` | `balanced-performance` | `performance` |

**Why the old tool showed the wrong mode:** it read `0x0555`/`0x054F`. Those
are a two-level power-tier flag (`01` for eco/quiet/balanced, `02` for
performance/turbo), not the mode.

## 2. What actually drives the LED

* **No EC GPIO drives it.** The three pins in the old notes are:
  * `GPA6`: PWM6 output (GPCR `0x1616` = `0x00`, alt function), a **fan PWM**. Its duty rises with the mode.
  * `GPD7`: alt function, ~50 % square wave, a **fan tachometer** input.
  * `GPM1`: an **eSPI/LPC bus** pin (random ~35 % activity).

  Sampled 4000× per mode: no EC GPIO and no PCH GPIO pad holds a steady value that changes with the mode.
* **The LED sits on the EC's SMBus channel D at 7-bit address `0x67`.** Channel D's
  only idle traffic is a temperature sensor at `0x4D`, read twice (3 ms apart) once per second.
  On every mode change the EC sends exactly this to `0x67`:

  | # | Protocol (HOCTL) | Cmd | Data |
  |---|---|---|---|
  | 1 | write word (`0x0C`) | `0x00` | `0xFC81` |
  | 2 | block write (`0x14`) | `0x03` | len 1, **[code 1–5]** |
  | 3 | write word (`0x0C`) | `0x00` | `0xE080` |
  | 4 | write byte (`0x08`) | `0x01` | `0xCD` (execute opcode) |

  The EC sends nothing to `0x67` between mode changes, so a code sent by hand
  should stay on the LED until the next mode change.

* **`0x67` is the keyboard lighting controller.** The Mode button is driven by the
  same chip. BIOS WMI method 20 (keyboard backlight) makes the EC send:

  | # | Cmd | Data | Meaning |
  |---|---|---|---|
  | 1 | `0x00` word | `0xF280` | select page F2 (keyboard zones) |
  | 2 | `0x03` block | 13 bytes: `R G B` ×4 zones, brightness 0–100 | zone colours |
  | 3 | `0x00` word | `0xFD81` | select page FD |
  | 4 | `0x03` block | `01 00` | effect = static (probably) |
  | 5 | `0x00` word | `0xE080` | commit |
  | 6 | `0x01` byte | `0xC2` | execute: apply keyboard |

  General pattern: `cmd 0x00` selects a page, `cmd 0x03` writes data into it,
  `0x00 = E080` commits, and **`cmd 0x01` is an execute opcode**. Without it nothing
  happens. `0xCD` = play Mode-indicator preset (this is what does the
  button + keyboard flash), `0xC2` = apply keyboard colours.

Channel D registers (ITE layout): `0x1C35` HOSTA, `0x1C36` HOCTL, `0x1C37` HOCMD,
`0x1C38` TRASLA, `0x1C39` D0, `0x1C3A` D1, `0x1C3B` HOBDB, `0x1C3D` pin control, `0x1C3E` HOCTL2.

### Mode-LED colours (observed)

| Code | Name in tool | Colour | Used by stock firmware for |
|---|---|---|---|
| 0 | `off` | LED off | – |
| 1 | `green` | green | Eco |
| 2 | `white` | white (slightly yellowish) | Quiet |
| 3 | `cyan` | cyan | Balanced |
| 4 | `purple` | purple | Performance |
| 5 | `pink` | pink | Turbo |
| 6 | `orange` | orange | – |
| 7 | `red` | red | – |
| 8 | `dim-cyan` | dim cyan | – |
| 9 | `dim-white` | dim white | – |
| 10–20 | (number) | white, getting brighter as the code rises | – |
| 27 | `dim-orange` | dim orange | – |
| 37 | `yellow` | yellow (also 45, 90, 126) | – |
| 39 | `salmon` | light salmon pink | – |
| 43 | `dim-blue` | dim blue | – |
| 46 | `lemon` | lemon yellow | – |
| 50 | `dim-purple` | dim purple (also 70) | – |
| 80 | `rose` | pinky red | – |
| 81 | `magenta` | pink with some purple | – |
| 83 | `light-orange` | light orange | – |
| 86 | `blue` | blue | – |
| 123 | `light-blue` | light blue | – |

Full scan of 21–255 by eye (`windows/palette_scan.ps1`). The palette has **128 entries**:
bit 7 is ignored, so 128–255 repeat 0–127 (128–135 = off, green, white, cyan, purple,
pink, orange, red). Most other codes are white or yellowish/bluish white at different
brightness; 42, 85 and 93 are off. More repeats: green 38/84/92, pink 41/44/88,
cyan 47/48/87/124, purple 49, orange 89, red 127, dim cyan 113. Any code 0–127 can
be given as a number.

### LED controller behaviour (tested)

* All writes are ACKed. Hand-sent codes stay on the LED until the next mode change.
* **Keyboard: any RGB, instant, no flash** with `--kbd-rgb` (page F2 + opcode `0xC2`). Confirmed visually.
  It goes straight to the chip, so the BIOS doesn't remember it: after a reboot or resume
  the keyboard returns to the colour the BIOS stored.
* **Mode button: presets only.** Page FC reads only the first data byte, as a preset number
  (`00 FF 00` → code 0 → off). The button is not a hidden 5th entry in page F2
  (tested: 16-byte F2 block, button unchanged).
* The flash is part of opcode `0xCD`. Page FC + data, with or without commit but
  without the `cmd 0x01` byte, does nothing. `0xCD` always plays the flash with the
  last-written preset number. Changing the page low byte (`FC80`), adding extra
  bytes, skipping the commit, or using pages FB/FE made no difference; it still flashed.
  Both the button and the keyboard flash. Sending the keyboard static-apply (`0xC2`)
  straight after `0xCD` does not cut the animation short. Neither does the button
  static effect `0xA0` (once, three times, or `0xA0` + `0xC2`), tested on Windows with
  `windows/reg_probe.ps1`: same flash, same final colour.
* Execute opcodes tested (preset in page FC, user watching):

  | Opcodes | Effect |
  |---|---|
  | `0xC0–0xCF` except `0xCD` | keyboard effects (static, flashing, breathing, some off); `0xC2` = static zones; `0xCF` = zone-by-zone boot animation. Button unchanged. |
  | `0xCD` | Mode-indicator preset **with flash** (button + keyboard) |
  | `0xD0–0xDF` | button never changed |
  | `0x30–0x33` | nothing visible with a new preset (`0x30` once looked dimmer) |
  | `0xFF` | looked like `0xCD` |
  | `0x00–0x93`, `0xB4–0xBF`, `0xE0–0xFE` | grouped sweep (16 at a time): no button change |
  | `0xA0–0xA3` | button **effects**, not colour (see below): `0xA1`/`0xA3` alone = breathing in the current colour; `0xA2` = **automatic colour cycle** (brighter/dimmer while drifting through colours, no flash), which keeps running with nothing sent. This is what made groups 12–14 of the first sweep look like flash-free colour changes. `--led-restore` stops it. |

  Keyboard effects decoded from WMI method 20: the colours go in page F2, page FD =
  `[01, speed]`, and the opcode is `0xC2 + mode` (`0xC2` static, `0xC3` breathing, …).
  The button family is `0xA0 + mode`, always sent after FD `[01, speed]` + commit `E080`.
  Effects switch **without a flash** (Windows tests, `windows/cycle_probe*.ps1`, `static_probe.ps1`):

  | Sequence | Button |
  |---|---|
  | FD + `E080` + `0xA0` | static in the latched colour; stops breathing or the cycle, no flash. Bare `0xA0` (no FD/commit) does nothing |
  | FD + `E080` + `0xA1` | breathing in the latched colour |
  | FD + `E080` + `0xA2` | colour cycle: fades orange → dim pink → white → orange → dim purple → dim cyan, brightness varies. Speed byte `00` ≈ 8 s per round, `09` ≈ 2.5 s, `20`/`80`/`FF` same as `09`. A new speed only applies when `0xA2` is re-sent; first FD byte `00`/`01`/`02` looks the same |

  The cycle runs inside the LED controller: 4 snapshots static vs 4 while cycling showed
  no EC change beyond normal sensor noise (XRAM `0x0000–0x0FFF`, PWM `0x1800–0x184F`,
  SMBus `0x1C00–0x1C4F`; only HOBDB held the speed byte).

  The **latched colour is set only by `0xCD`** (which plays the flash). FC preset + FD +
  `E080` + `0xA0` or `0xA1` keeps the old colour, also when sent while cycling. RGB data in
  page FC (`00 FF 00 00`) or 3 extra bytes after the F2 zones does nothing either.
  Page FD values have no effect on the `0xCD` flash (A/B tested `00 00` vs `01 00`).

  So far the only way to choose a specific button colour is `0xCD`, which flashes.

  The BIOS LED-group WMI methods don't reach it either. Acer's `AcerECLightbarController.dll`
  drives logo/lightbar RGB with `SetGamingLEDColor` (method 12, u64 `[group, R, G, B,
  brightness, speed]`) and `SetGamingLEDBehavior` (method 10, `[group, 0, effect]`,
  `0x10001` = group 1 static). On the PHN16-71, groups 1, 2 and 4 changed nothing
  visible and made the EC send nothing on SMBus-D (`windows/led_probe.ps1`). Acer's
  software does have an RGB `AcerHIDModeKeyLED` device, but only for models whose EC
  exposes a HID lighting interface, which this one doesn't.

  Keyboard-effect method 20 (`[effect, speed, brightness, 0, direction, R, G, B, 3, device]`,
  layout from the PHN16-72 project) with other device bytes (`windows/device_probe.ps1`):
  device 2 (light bar) is rejected (status 1, `GetGamingKBBacklight(2)` also 1); device 3
  (global) is just the keyboard again. Neither reaches the Mode button. **Warning:** a static
  effect through method 20 ignores its RGB bytes and uses the stored zone colours, which were
  black here, so the keyboard went dark. `windows/kbd_restore.ps1` (Administrator) puts back
  the effect that was read before the test.

  Page sweep (`windows/page_sweep.ps1`): RGB pattern `FF0000`×4 + `64` written to the
  untried pages `F0 F1 F3–FB FE FF` (low byte `80`, then `81`), then FD `01 00`, commit
  `E080` and one opcode. All writes ACKed. `0xA0`: no change. `0xA1`: slow breathing in
  the stock cyan, i.e. the colour store isn't in these pages. `0xC2` with low byte `80`:
  **some keyboard zones changed**, so one of these pages also feeds the keyboard static
  apply (not narrowed down). Untried: pages `E1–EF` and below `E0`, and opcodes `0x94–0x9F`
  and `0xA4–0xB3` combined with RGB pages.
  The palette has 128 presets (see the colour table above), which is the full set of
  colours the button can be given.

  The rest can be swept by hand with
  `sudo ./predator_i2ec --op-sweep <from> <to> [sec] --yes` (Ctrl+C when the button
  changes; it restores the keyboard and button on exit).
* Reads mostly return `0xFFFF` or NAK, so the controller is effectively write-only.
  cmd `0x06`/`0x08` return some internal buffer byte, **not** the colour index: on Windows
  they read `03` right after a preset write, then `21`, `7F`, `E0` after further read sweeps
  while the button stayed unchanged. There is no readable palette.
* BIOS WMI (GUID `7A4DDFE7-…`, ACPI `WMBH`): method 7 = get zone colour
  (`[zone mask]` → `[status, R, G, B]`, zones `01 02 04 08`), method 21 = get effect,
  method 20 = set effect `[mode, speed, brightness, 0, direction, R, G, B, 0, 1, 0…]`
  (16 bytes). Method 6 (`[mask, R, G, B]`) returns OK but sends nothing by itself.
  `acer_gkbd/` is a small debugfs kernel module for calling these
  (`insmod acer_gkbd.ko`, then `echo "21 01" > /sys/kernel/debug/acer_gkbd/call`).

## 3. Tool

```bash
make            # or: gcc -O2 -Wall -Wextra predator_i2ec.c -o predator_i2ec

sudo ./predator_i2ec --probe            # current mode, cross-checked from 5 sources
sudo ./predator_i2ec --watch            # live log of every mode change (button or software)
sudo ./predator_i2ec --mode turbo       # eco | quiet | balanced | performance | turbo
sudo ./predator_i2ec --cycle 5          # 5 s in each mode (watch the LED), then restore
sudo ./predator_i2ec --sniff 10         # log SMBus-D transactions while you press the button
sudo ./predator_i2ec --dump 0500 0600   # read-only XRAM hex dump
```

`--mode` writes `/sys/firmware/acpi/platform_profile`. That makes acer-wmi call
WMI → BIOS SMI → EC, so the EC runs its full mode-switch routine: fan tables,
GPU/CPU power limits, and the LED. Writing the mode byte straight into EC RAM
does **not** do this. Plain shell works too:

```bash
echo balanced-performance | sudo tee /sys/firmware/acpi/platform_profile
```

With `acer_wmi` loaded (`cycle_gaming_thermal_profile=Y`, the default), the
physical Mode button is handled by the kernel and cycles the same profiles.

### Experimental: custom LED code

```bash
sudo ./predator_i2ec --led-restore --yes          # re-send the current mode's stock code
sudo ./predator_i2ec --kbd-rgb FF0000 00FF00 0000FF FFFFFF 100 --yes   # keyboard, 4 zones, any RGB
sudo ./predator_i2ec --kbd-rgb ff8800 60 --yes    # whole keyboard orange, 60 %
sudo ./predator_i2ec --led-raw red --yes          # button preset by name (or number: --led-raw 7)
sudo ./predator_i2ec --led-raw cyan 0x30 --yes    # 2nd arg = execute opcode (0x30 looked dimmer)
sudo ./predator_i2ec --lt w00:FC81 b03:07 w00:E080 c01:CD --yes   # raw transactions
sudo ./predator_i2ec --led-raw off --yes          # LED off
sudo ./predator_i2ec --led-scan 21 40 4 --yes     # explore more codes, 4 s each

# keep custom colours: re-applies your colour whenever the EC changes mode
sudo ./predator_i2ec --led-daemon turbo=red,performance=orange,quiet=white:0x40 --yes

# button effects, no flash (colour = the last preset; cycle has its own colours)
sudo ./predator_i2ec --led-effect breathing --yes # pulse in the current colour
sudo ./predator_i2ec --led-effect cycle 9 --yes   # colour cycle, speed 0 (~8 s/round) to 9 (~2.5 s)
sudo ./predator_i2ec --led-effect static --yes    # back to steady
```

`--led-effect` doesn't change the colour; set that first with `--led-raw` (one flash).
A mode change sends the EC's own preset, which returns the button to static.

These replay the EC's own 4-transaction sequence (table above) with your
code/brightness, timed right after the EC's temperature poll. A plain
`--led-raw` lasts until the next mode change; `--led-daemon` re-applies your
choice within ~0.2 s of each change and restores stock colours on Ctrl+C.
While the daemon runs, other `predator_i2ec` commands are locked out (they
share the I/O ports); use `cat /sys/firmware/acpi/platform_profile` to check the mode.

If the LED or EC ever acts strangely, switch modes once (`--mode balanced`).
If that doesn't help, shut down fully and power on again.

Run the daemon at boot (installed system only, not the live USB) with
`/etc/systemd/system/predator-led.service`:

```ini
[Unit]
Description=Predator Mode-LED custom colours
After=multi-user.target

[Service]
ExecStart=/usr/local/bin/predator_i2ec --led-daemon turbo=red,performance=orange --yes
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

### Windows

The same source builds on Windows with MinGW-w64. No extra driver is needed:

```bash
mingw32-make    # or: gcc -O2 -Wall -Wextra predator_i2ec.c -o predator_i2ec.exe -lole32 -loleaut32 -lwbemuuid
```

Run it from an Administrator terminal. The commands are the same as above, without `sudo`
(PowerShell needs the `.\` prefix):

```powershell
.\predator_i2ec.exe --probe
.\predator_i2ec.exe --led-raw red --yes
.\predator_i2ec.exe --led-daemon turbo=red,performance=orange --yes
```

* **Port I/O** goes through `inpoutx64.sys` (`\\.\inpoutx64`), which PredatorSense
  installs and starts at boot. Around each EC access the tool takes the
  `Global\Access_ISABUS.HTP.Method` mutex, the one HWiNFO and LibreHardwareMonitor-based
  tools use, so their Super I/O probing can't interleave with the I2EC sequence.
* **Mode** (`--mode`, `--cycle`, and the profile line of `--probe`) uses BIOS WMI
  `ROOT\WMI:AcerGamingFunction`, `SetGamingMiscSetting` / `GetGamingMiscSetting` with
  setting `0x0B`. That is the same call acer-wmi makes on Linux, so the EC runs its full
  mode switch. WMI needs Administrator. PredatorSense may not show the new mode until it is reopened.
* **Tested on Windows 11** (BIOS V1.20): `--probe` (including the WMI profile line),
  `--dump`, `--mode` (WMI, confirmed by EC `0x54`), `--led-raw`, `--led-restore`,
  `--led-effect` (sequences confirmed by eye in the probe scripts), and `--led-daemon`
  applying and restoring its colour. Not tested yet: the daemon following
  a press of the physical Mode button (PredatorSense handles the button on Windows).
* The `inpoutx64` device opens without elevation, so EC reads and LED writes also work
  from a normal terminal. That is how Acer's driver is set up: any program can do raw port I/O.
* Closing the console window or ending the process skips the restore. The custom
  colour then stays until the next mode change. Stop the daemon with Ctrl+C instead.

Start the daemon at logon (Task Scheduler, highest privileges):

```bash
schtasks /create /tn "Predator LED" /sc onlogon /rl highest /tr "\"C:\path\to\predator_i2ec.exe\" --led-daemon turbo=red --yes"
```

`windows/PredatorLights.ps1` is a WMI-only alternative (mode, keyboard RGB and effects,
no Mode-button presets).

## 4. Corrections to the old notes / tool

| Old claim / behaviour | Reality |
|---|---|
| `0x0555` = mode (1 Quiet, 2 Eco, 3 Bal, 4 Perf, 5 Turbo) | Mode is EC `0x54` (`0x0554`), encoded 0 Bal, 1 Quiet, 2 Perf, 3 Turbo, 4 Eco. `0x0555` is a power-tier flag. |
| GPA6/GPD7/GPM1 are the LED's R/G/B | Fan PWM, fan tach, eSPI bus. Writing GPDR on alt-function pins does nothing. |
| `--mode` writes `0x054F`/`0x0555` | Doesn't run the EC's mode-switch routine, so the LED and fans don't follow. Removed. |
| `--restore` writes `0x1823`, `0x0A07`, `0x1610`, `0x1612`, `0x0500` | Arbitrary writes (`0x1610`/`0x1612` are GPIO-config registers for GPA0/GPA2, and `0x0500` is ACPI EC byte 0, currently `0x04`). Removed. |
| `0x1520`/`0x1522` = SMBus channel C | The IT5570 SMBus controllers are at `0x1C00+`. |

## License

GPL-2.0 (see `LICENSE`). That covers the tool, the scripts and `acer_gkbd/`, which as a
Linux kernel module is GPL-2.0 anyway.
