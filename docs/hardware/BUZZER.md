# Built-in piezo buzzer

The BE-300's piezo is separate from headphone PCM. Glaze now drives the
second BLG PWM channel for system clicks, lock/unlock, notifications, calendar
alerts, timer alarms and tone previews. Music, games and arbitrary WAV/PCM
streams keep their headphone path. The piezo versions of the sound presets
are short monophonic note patterns, not playback of the WAV recordings.

The existing Sounds switches and tone selections apply. Master Volume zero
mutes both outputs; other volume levels set the buzzer pulse width from 5%
to 50%. This follows the recovered driver's duty calculation, but perceived
loudness across physical devices has not been calibrated. Alerts take priority
over clicks. Timer patterns repeat with a gap, stop on dismissal, and expire
after one minute. Changing volume updates a currently sounding note.

Initialization stops inherited buzzer output without playing a startup tone.
The kernel stops each requested note on a timer deadline, even if the audio
server stops servicing its pattern. Notes are bounded to one second each.
The audio server alone can use the appended `SYS_BUZZER_TONE` syscall.

## Register evidence

Recovered from the user's unmodified Windows CE NAND (`300.bin`, SHA-256
`cfafca7f1bdcdd82f9a9980b4a684a9830562c8b8f19bc1287131c83a674d6ec`),
`bltinbuz.dll` preferred base `0x018F0000`. No vendor binaries are distributed.

| Physical address | Meaning | Evidence |
| --- | --- | --- |
| `0x0A000980` | Backlight enable, **not** buzzer enable | SPL/ddi traces; see `DISPLAY.md` |
| `0x0A000984` | Shared BLG configuration; buzzer ORs `0x0202` | `018F131C`, `018F1638..1640` |
| `0x0A000988/98C` | Backlight period/duty, left unchanged by buzzer | SPL/ddi traces |
| `0x0A0009A0` | Buzzer channel enable; zero stops, one starts | `018F1304`, `018F1624`, `018F1678`, `018F2190` |
| `0x0A0009A4` | Buzzer period | `018F1658` |
| `0x0A0009A8` | Buzzer duty | `018F166C` |
| `0x0A001128` | Shared BLG clock gate, bit 0 | SDK CMM range (`be300/docs/hardware/hardware.txt:188`), mapped-gate RMW at `018F1604..1618` |

`018F119C` computes `period = 1411200 / Hz - 1`, returning a 16-bit value.
`018F11F8` computes `duty = period * (100 - pulse_percent) / 100 - 1`.
The normal driver uses 50%. The corresponding counter intervals include
the register's +1; do not round the host oscillator to an integer Hz.

The captured Windows CE startup trace independently confirms writes from
`018F1304/131C/1324/132C` to physical offsets `9A0/984/9A4/9A8`, with values
`0/0202/0/0`. The old extracted `.data` sections are ROM-compressed;
their raw copied bytes are not initialized pointer/resource values.

Glaze follows the playback order: stop the channel, enable the shared clock,
OR the configuration bits, write period and duty, then enable the channel.
Display power-off preserves the shared clock/configuration while a buzzer
note is active. Wake restores the backlight mode without replacing another
channel's configuration changes. An alarm can therefore sound with the
backlight off.

## Emulator and the old boot chirps

The former emulator heuristic treated backlight initialization at `0980`
as audio and guessed pitch from backlight period `0988`. That heuristic was
removed with the recovered LCD initialization work (`be300` commit
`d4e6bab4`). The separate LCD/NAND overlap fix kept `0C00..0C4F` out of
the legacy NAND recovery dispatcher. The buzzer at `09A0` is outside that
NAND buffer window.

The new emulator models the actual `09A0/9A4/9A8` PWM channel and shared
configuration/clock. Backlight changes cannot start a tone. Native SDL mixes
the continuous PWM with headphone PCM in its audio callback. The web backend
mixes it into drained stereo frames, bounded by elapsed host time; it does
not queue an invented default pulse. Host mute leaves register behavior
unchanged. See `be300/docs/BE300_BUZZER.md` for backend details.

## Validation and physical test

```sh
make -C host buzzer-driver-test audio-driver-test audio-volume-test display-test
make -j4 all glazeboot
```

Native tests run the production kernel driver with fake MMIO, checking
permissions, frequency/duration bounds, recovered counts, timed stop and
preservation of backlight and PCM words. Sound tests check startup silence,
click/alert priority, note progression, dismissal and volume/mute. Display
tests check shared-clock retention while dark and configuration preservation
on wake. Emulator tests check the PWM register path, no backlight-generated
tones, stereo mixing, host mute and bounded web synthesis.

Physical piezo validation is still required. Deploy matching kernel and ROMFS
from this build using GlazeBoot/USB, keeping NAND read-only. Open
**Settings → Sounds**, set Volume above zero and preview the notification
tones without headphones. Check keys/lock with their switches enabled, then
start a timer and turn the screen off. Confirm it continues sounding, stops
on dismissal and remains silent at Volume zero. Headphone playback should
continue to work independently.
