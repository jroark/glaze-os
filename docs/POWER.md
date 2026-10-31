# Battery, charging and idle power

Glaze reads the battery, the AC adapter and the charger as Windows CE's
battery driver does. It shows them in the status bar and on Settings →
Battery, warns as the battery runs down, and puts the CPU in VR4131
STANDBY whenever no thread is runnable.

## Hardware, from Windows CE

The battery driver is compiled into `gwes.exe` (`BatteryPDDGetStatus`,
0xB02CC–0xB09DC in be300-modules `03_gwes.exe.bin`, VA = file offset +
0x10000). The EEPROM transport is `eeprom.dll` (`E2pRead` 0x01A210FC,
transport 0x01A212F4; be300 `docs/WINCE_DISPLAY_CONTROLS.md`).

| Signal | Where | Meaning |
|---|---|---|
| Adapter | GIUPIODL `0x0F000144` bit 12 | 1 on battery, 0 with the adapter in |
| Charger | GIUPIODL bit 2 | with the adapter: 1 charging, 0 charged (CE reports 100 %) |
| Voltage | PIU A/D port, companion `0x0A000390` & 0xFFF | 12-bit; CE dumps read 0xC6D–0xE22 |
| Calibration | EEPROM word address 0x94/0x95 (selector 0x4A) | max, min bytes |
| Battery low | PMUINT bit 1 BATTINTR (VR4131 UM 12.2.1) | source on the BE-300 unknown |

**A sample** (gwes 0xB0348–0xB0590) runs only when PIUCNT bits 12:10 read
Standby (0x400) or WaitPenTouch (0x1000), so never during a touch. The
sequence is:

1. Clear PADATSTART, then PIUSEQEN.
2. Drive GPIO8 low and wait 10 ms. CE never sets GPIO8 high again.
3. Save 0x314 and 0x318, then write `0x314 = 5` and `0x318 = 0x14E`.
4. Select PIUMODE 01 (PIUCNT bit 3).
5. Mask PENCHG and bit 6 in PIUINT, and acknowledge bit 6.
6. Set PIUSEQEN and poll PIUINT bit 6, up to 3000 times.
7. Read 0x390.
8. Undo the steps above and restart the touch scan.

CE writes PIUINT back with its pending byte, which acknowledges any touch
event still pending. Glaze writes only the mask byte and bit 6.

**Calibration** (gwes 0xB0AF0) decodes the two bytes as:
- max = `(b0 << 2) | 0xC02`
- min = `(b1 << 2) | 0xC02`, or `| 0x802` if b1 ≥ 0x7F
- If b1 ≥ b0, the defaults are 0xD96 (4.2 V) and 0xBF8 (3.7 V), the service
  manual's "AD Revision Value" points.

**Bands.** With span = max − min, an averaged reading at or below each edge
gets that band:

| Reading at or below | Band |
|---|---|
| min − span/10 (about 3.65 V, the manual's warning) | 10 % |
| min | 25 % |
| max − 4·span/5 | 50 % |
| max − 2·span/5 | 75 % |
| above | 100 % |

**Averaging.** The last ten readings are kept. Once there are ten, the
largest is dropped before averaging.

**Critical** is two 10 % readings on battery since the adapter was last in.

The CMCU traffic (0x0A007800) is the watchdog, not a battery query; see
`kernel/drivers/cmcu.c`.

## Kernel

`kernel/drivers/battery.c` does everything from the 100 Hz tick, with no
sleeping:
- It holds the adapter and charger lines steady for 0.5 s before reporting
  a change.
- It samples the battery every 2 s. The 10 ms settle after GPIO8 is two
  ticks, with the touch sequencer stopped as CE stops it. If the touch
  driver restarts the sequencer during the settle, the sample is skipped
  and retried 0.25 s later.
- It notifies the compositor's input port with `GZ_NOTIFY_POWER` when the
  flags, band or level change.

The arithmetic is in `lib/include/glaze/battery.h`. That header also gives
a 0–100 `level` for display. It is piecewise linear through CE's own scale:
- 0 % at 3.4 V. This is min − 3·span/5, an edge gwes computes at 0xB0CC0
  but never uses, and the manual's cut-off.
- 10, 25, 50, 75 and 100 % at the band edges (3.65, 3.7, 3.8, 4.0 and
  4.2 V).

CE's band is then the level rounded up to the next band. With the emulator
at `--battery 40`, Glaze shows 40 % and Windows CE 50 %.

The calibration is read once at boot, bit-banged as eeprom.dll does it:
1. Write `0x0A001124 = 0`, `A140 = 1` and `A194 = 0` (CE's idle dump shows
   all three).
2. Drive SCL at `A19C` and SDA at `A1A0`, and sample SDA at `A1A4`.
3. Send device 0xA0, word address 0x94, a repeated start, then 0xA1.

A missing acknowledge or unusable bytes mean CE's defaults are used.

`SYS_POWER_STATUS` (`gz_power_status()`) is unprivileged and copies out
`struct gz_power_status` (`glaze/abi.h`). Callers pass its size, and other
sizes fail with `GZ_EINVAL`. The struct holds:
- the flags (`VALID`, `AC`, `CHARGING`, `LOW`, `CRITICAL`, `BATTINT`,
  `CAL_EEPROM`)
- CE's band (`percent`) and the `level`
- the last and averaged readings and the calibration
- the sample count
- the STANDBY time and count

## Idle STANDBY

When nothing is runnable, the idle thread executes STANDBY
(`vr41xx_standby()` in `kernel/mips.h`, VR4131 UM 12). The pipeline clock
stops until any interrupt, and the 100 Hz tick wakes it at the latest. That
tick also polls every device without an interrupt of its own.

A real VR4131's STANDBY sets Status.IE itself; Linux's `vr41xx_cpu_wait`
relies on that. be300's gxemul used to run STANDBY as WAIT and never set IE.
The idle loop therefore enables interrupts first and checks `need_resched`
once more. An interrupt in those few instructions delays its thread by at
most a tick.

`trap()` ends the accounting at the first interrupt (`sched_standby_wake`).
Settings → Battery shows the idle share as "CPU Idle". To build without
STANDBY when bisecting on hardware, use `IDLE_STANDBY=0`, then rebuild
`kernel/sched.o`.

## SpringBoard and Settings

**Status bar.** The battery is filled to `level`:
- amber at a level of 20 % or less and red when critical, both on battery
  only
- with a bolt while charging
- full until the first sample

With Settings → Battery → Percentage on (`sb.battery_pct`,
`glaze/power.h`), the level is drawn as a percentage beside the icon, and
the card icon moves left.

**Warnings** (`servers/springboard/power.c`):
- A note from Settings at a level of 20 % and again at 10 %, once per
  discharge. It shows as a banner, or on the lock screen while locked.
- While critical, the screen timeout is at most 15 s.
- After 30 s critical, SpringBoard sends the front app to the background,
  which makes it save, stops sounds, turns the screen off and posts
  "Battery Critically Low".
- No power-off control is known, so this is a safe sleep. The battery's own
  cut-off (about 3.40 V, per the manual) ends it if the adapter never comes.
- BATTINTR counts as critical only together with CE's 25 % band, because its
  source isn't known.

**Settings → Battery** (the last row of Settings) shows:
- status and level
- the Percentage switch
- the latest and averaged readings
- calibration source and range
- CPU idle

## Emulator and tests

be300 models the battery (`src/hw/battery.c`), the A/D port (`src/hw/piu.c`)
and the EEPROM (`src/hw/eeprom.c`). It takes these options:

| Option | Effect |
|---|---|
| `--battery PCT` | battery level, on the same scale (CE's default calibration) |
| `--battery-state ac\|charging\|battery` | adapter and charger state; `ac` is the default |
| `--battery-drain N` | N % per emulated minute; charging ends in `ac` |
| `--eeprom-cal B0,B1` | calibration bytes; without it the EEPROM is erased, so the defaults are used |

`tests/run.sh` passes `BE300_ARGS` through to the emulator.

- `make -C host battery-test` runs the arithmetic and the driver on fake
  registers.
- `BE300_ARGS="--battery 60 --battery-state battery" tests/run.sh tests/t_battery.txt 16`
  opens the Battery page and turns on Percentage.
- `--battery 25 --battery-state battery --battery-drain 20` over about 150 s
  logs both warnings and then `springboard: battery critical, sleeping`.

## Not known yet

- The ADC's reference voltage and divider. The volts above come from CE's
  calibration points.
- Real units' EEPROM bytes at 0x94/0x95.
- Which GPIOs carry the manual's VDET0B, VDET2 and VDETCF detectors.
- What drives BATTINTR.
- Whether 0x0A00A1xx sits behind a clock gate at a power-on boot. CE's dump
  and ROM boots read it without hanging, but a battery-pull boot hasn't
  been tried.
