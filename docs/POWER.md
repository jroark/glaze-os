# Battery, charging, idle power and power off

Glaze reads the battery, the AC adapter and the charger as Windows CE's
battery driver does. It shows them in the status bar and on Settings →
Battery, warns as the battery runs down, and puts the CPU in VR4131
STANDBY whenever no thread is runnable. Holding Power, or leaving the
screen off, powers the device off as Windows CE does: Goodbye, then
SUSPEND or HIBERNATE until Power or an alarm (see [Power off](#power-off)).

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
- After 30 s critical, SpringBoard powers off by Suspend, as when Power is
  held (the front app saves first), with "Battery Critically Low" waiting
  on the lock screen. The battery's own cut-off (about 3.40 V, per the
  manual) ends it if the adapter never comes.
- BATTINTR counts as critical only together with CE's 25 % band, because its
  source isn't known.

**Settings → Battery** (the last row of Settings) shows:
- status and level
- the Percentage switch
- the latest and averaged readings
- calibration source and range
- CPU idle

## Power off

### Windows CE

Found by disassembling the stock modules (be300-modules; nk.exe is based
at 0x80060000):

- **OEMPowerOff** (nk.exe 0x800792AC-0x80079598) ends in HIBERNATE. Before
  it, CE:
  1. saves the registers, CP0, TLB and interrupt masks to PA 0x2000-0x22E4;
  2. writes a "RAM is valid" pattern at PA 0x2700;
  3. masks and acknowledges the companion sources (0x30 to 0x0A000010-4C);
  4. writes back the D-cache;
  5. turns the LCD off (0x80079BB4: 0xC48 = 0x10F, the backlight 0x980/0x984
     and its gate 0x1128 off, 0xC48 = 0x101, then 0, 0xC30 = 1, gate 0x110C
     off);
  6. zeroes the companion gates 0x1100-0x1130 and CMUCLKMSK;
  7. writes 0xFB3B to PMUINT.

  The wake is a cold reset: ROM, SPL, then NK's reset code checks the
  pattern and resumes, or hibernates again unless Power is held (the button
  word 0x0A00A042 | A044, bit 0x8000), the RTC alarm or GPIO1 woke it
  (0x80077820-0x80077944). A power-switch wake sets no PMUINT bit (UM
  12.2.1). SUSPEND appears only in CE's idle loop.
- **Goodbye**: PowerOn.dll's POWEROFF_WINDOW, a full-screen black popup
  with "Bye..." in white, the system font at 1.25× and bold, up while the
  registry and alarms flush.
- **The CMCU watchdog** is stopped before sleeping and started after
  (cedmbltin.dll CDM_PowerDown 0x019F1430, CDM_PowerUp 0x019F1454): stop is
  ARG0 0x7804 = 0x35, CMD0 0x7800 = 0x5C until STATUS & 7 reads 0.
  Without it a real BE-300 would reset 150 s into a sleep.

### Glaze

`SYS_POWER_SLEEP(mode, alarm_s)` (`gz_power_sleep`, privileged; GZ_SLEEP_*
and GZ_WAKE_* in `glaze/abi.h`) is `kernel/drivers/power.c`:

1. It stops what can't run through a sleep:
   - the audio DMA (`audio_suspend`, restarted where it was after);
   - the piezo;
   - a battery sample in progress;
   - the touch source;
   - the panel, by CE's sequence (`display_sleep`);
   - the CMCU watchdog (`cmcu_stop`).
2. It sleeps:
   - **Suspend.** VR4131 SUSPEND, with only the power switch's interrupt
     (SYSINT1 POWERINTR), GIRQ0's button source and the ETIME compare
     unmasked. A wake by another button suspends again. Interrupts are
     turned on in the same asm block as SUSPEND, and `power_trap` returns
     past it, so an interrupt in between can't leave SUSPEND waiting with
     interrupts off.
   - **Hibernate.** VR4131 HIBERNATE through the SPL (below). A RAM boot
     from Windows CE suspends instead, since CE's own SPL would start CE.
     If an interrupt was pending, HIBERNATE does nothing, and Glaze
     suspends.
3. It carries the monotonic clock across on the RTC's ETIME
   (`timer_suspend`/`timer_resume`), since Count stops, resets, or wraps
   after about 413 s.
4. It starts everything again: after a Hibernate, the companion gates,
   `irq_init`, the CMCU, USB (the host's session is lost), the PIU, the
   audio block and the PCMCIA socket (whatever is in it is inserted afresh,
   and the file server remounts the card).
5. It returns GZ_WAKE_POWER or GZ_WAKE_ALARM, and the mode used.

**Hibernate and the SPL** (`kernel/resume.h`, `kernel/power_resume.S`,
`boot/spl.c`):
- `power_save` records the callee-saved registers, Status and EntryHi in a
  resume record at PA 0xE000.
- The boot ROM overwrites RAM on its way to the SPL. In the emulator,
  running the stock ROM, it changed PA 0x0000-0x0FFF, 0x10000-0x18FFF and
  0x20000-0x20FFF (the vectors and the kernel's own text: CE's kernel sits
  above that, Glaze's at 0x80010000), and the first word of the page at
  each power of two from 0x40000 (RAM sizing).
- So the kernel copies the first 48 pages and every probed page below its
  end to spare pages listed in the record. `mm.c` keeps the probed pages
  above the kernel, the SPL's image (0x00F00000) and its stack (below
  0x00FF0000) out of use on NAND boots.
- The SPL, first thing:
  - boots afresh after a real reset (PMUINT RSTSW, RTCRST, TIMOUTRST or
    BATTINH);
  - hibernates again unless Power is held or the alarm in the record is
    due;
  - otherwise copies the saved pages back and checks that the kernel's
    text and read-only data (`__text_start` to `__ro_end`) sum to the
    record's `text_sum`, which the kernel computes just before it
    hibernates. The SPL isn't built against a kernel (docs/OTA.md);
  - then sets HALTIMERRST and jumps to `power_resume_entry`, which redoes
    `start.S`'s VR4131 cache workaround (`cpu_cache_setup`), restores
    Status and returns from `power_save`. The kernel flushes the TLB.
- The SPL logs why it didn't resume, once serial is up.
- `HIBERNATE_VERIFY=1` (rebuild `kernel/drivers/power.o` after changing
  it) sums every page in use before and after and logs any that changed.
  It reported none in the emulator.

**SpringBoard** (`servers/springboard/main.c`, `idle.c`):
- A Power press locks on release. Holding it `SB_POWER_HOLD_MS` (1.5 s)
  powers off.
- Powering off:
  1. Locks the screen.
  2. Pauses the player.
  3. Shows "Goodbye" (white `FONT_B20` on black) for a second.
  4. Gives the front app DEACTIVATE, so it saves and suspends itself; it
     stays in front under the lock.
  5. Calls `gz_power_sleep` with the next notification's time as the alarm.
- At the wake it gives the app ACTIVATE and shows the lock screen (the
  passcode still applies). A note that woke it fires there.
- With the screen timeout on, it also powers off by itself once the screen
  has been off for the Sleep setting's delay.

**Settings → Battery → Sleep** writes the `power.sleep` preference
(`struct power_sleep`, `glaze/power.h`) and notifies SpringBoard with
`SB_NOTIFY_POWER`:
- On Battery and Plugged In each have a mode (Suspend, Hibernate or Never)
  and a delay after the screen goes off (at once, 1, 5, 15, 30 or 60 min).
- The defaults are Suspend at once on battery, and Suspend after 15 min
  plugged in.
- Holding Power uses the source's mode, or Suspend when it is Never.

### Emulator and gaps

- be300 runs SUSPEND as WAIT, and F12 wakes it through GIRQ0. Its
  HIBERNATE cold-resets at once with RAM kept, so the SPL's
  hibernate-again loop models the wait (each pass is a ROM load of the
  SPL).
- Its ETIME compare interrupt is delivered only while RTCL1 is stopped,
  which Glaze never starts.
- Its PMUINTREG bits don't follow the UM (RTCRST at bit 0, RSTSW at 2), and
  its CMCU STATUS always reads 0.
- Scripted input (`INPUT_SCRIPT`) can't run while asleep, so a sleep in such
  a build also wakes for the script's next event, as a Power press.
  `<ms> key power <held_ms>` holds a key.
- Not checked on hardware yet:
  - whether real SUSPEND/HIBERNATE wakes on POWERINTR as expected;
  - whether the CMCU stop answers;
  - which RAM the real ROM touches (run a `HIBERNATE_VERIFY=1` NAND image);
  - the current drawn in each mode.

  CE zeroes every companion gate before HIBERNATE; Glaze leaves them as
  they are (the gate bus-stall hazard), which may cost power.

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

- `tests/run.sh tests/t_poweroff.txt 40` types a note, holds Power, wakes
  and finds it again. `tests/t_hibernate.txt` (40 s) selects Hibernate in
  Settings and resumes through the SPL. `tests/t_poweroff_alarm.txt` (90 s)
  wakes on Clock's timer.
- `make -C host idle-test` covers the hold and the auto-sleep deadline, and
  `boot-init-test` covers the SPL's resume decisions and the clock across a
  sleep.

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
