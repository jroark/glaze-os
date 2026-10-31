# BE-300 NAND boot initialization audit

Audit date: 2026-10-29. Target: stock masked ROM → Glaze SPL → Glaze kernel.
Glaze source baseline `bad936b`; sibling emulator baseline `aca11615` (working
trees may contain newer edits). This is an audit, not a flash procedure.

**NAND flashing readiness is unresolved.** Confirmed initialization fixes are in
place. A subsequent blocker pass added mapped, ECC-checked immutable boot reads
and resolved the ROM record framing and several initialization ownership
questions. Physical image generation, conservative bad-block preservation and
ROM/OOB integration tests are now implemented. Physical CF recovery and the raw
programming/restore path still need closure; see [current status](PHYSICAL_NAND.md).

The matrix records the initial findings. See **Implementation follow-up** below
and [ROM handoff and protected boot content](NAND_ROM_HANDOFF.md) for current
results, hardware validation and the remaining write-side blockers.

## Evidence and boot ownership

Glaze links below are relative to this document. `E:` paths are relative to the
sibling `../be300` repository. Routine addresses refer to the original stock
image, not Glaze symbols. Addresses in tables are physical unless prefixed with
an explicit KSEG virtual address. Access widths are part of the contract.

Evidence levels:

- **Direct:** current source or instructions decoded from the raw ROM/SPL.
- **Corroborated:** existing hardware captures or reverse-engineering notes;
  distinguish post-CE state from reset state.
- **Open:** required ownership, reset value, or hardware semantics not established.

The raw 16 KiB ROM is `E:docs/hardware/be300_boot_rom.bin`, SHA-256
`dcef0dc4b672596a4021a4e005d149835761a5906634028e7f5a4f008f31c9f3`.
The stock SPL records were read from
`E:ce/restore_images/All_nand_300.bin` at `0x4000`, reconstructing their load
addresses before decoding as little-endian MIPS32. No proprietary bytes are
included here. The checked-in `be300_boot_rom.dis` has misleading addresses and
byte interpretation; use the raw binary and dump text instead.

### Confirmed ROM work: do not count these as missing Glaze routines

- Reset entry sets CP0 configuration and calls cache/tag setup. See
  `E:docs/ROM_SPL_HANDOFF.md`, reset flow `0x9FC002F0` and `0x9FC006F0`.
- Raw ROM `0x9FC005A0..0x9FC00620` writes halfword bus/SDRAM settings:
  `0x0F000000`, `004`, `006`, `008`, `00A`, `016`, then `400`, `402`, `404`,
  `408`. Values come from ROM tables at `0x9FC00624/638`; its branch selection
  and resulting values must be captured at handoff, not guessed from CE dumps.
  Stock SPL `0x80F025A0` repeats substantially the same routine.
- Raw ROM `0x9FC0064C` checks clock divisors through halfword `0x0F000014`,
  conditionally writes `0x0F0000CC`, waits using RTC, and executes a low-power
  instruction. The stock SPL has a related routine at `0x80F0264C`.
- ROM `0x9FC00734` sets PMUCNT bit 2 using halfword accesses at
  `0x0F0000C2`, then performs RAM pattern/address tests. The basic HALTimer
  acknowledgement is therefore ROM-provided as well as repeated by Glaze.
- The ROM has to establish enough SDRAM and NAND access to load the SPL.
  This proves basic functionality at handoff, **not** every peripheral's final
  configuration or permission to change NAND transaction framing.

The ROM MIPS16 loader/dispatcher remains a boundary in this audit. Existing
`ROM_SPL_HANDOFF.md` identifies `0x9FC00C21/0x9FC00C85` and mailboxes at physical
`0x2400/0x24FC`, but does not isolate every initialization block executed before
SPL entry. No claim of exhaustive hardware coverage is justified until that
path's MMIO is accounted for. In particular, distinguish setup needed by the
ROM itself from state it leaves enabled for its replacement SPL.

### Initial Glaze ordering and RAM-boot contrast

[SPL entry](../boot/spl_start.S) disables interrupts, assigns a stack at
`0x80FF0000`, and calls C using cached code/stack addresses. Its first action
in [spl_main](../boot/spl.c) is unbounded UART output, then kernel reads through
the NAND transfer engine, then a direct kernel jump with arguments cleared.
It neither returns through the ROM mailboxes nor runs the stock SPL's setup.

[Kernel entry](../kernel/start.S) switches uncached for the VR4131 cache erratum
workaround, flushes both caches before/after changing Config, installs a stack,
and clears BSS. [kmain](../kernel/main.c) acknowledges HALTimer, prints, sets up
traps/memory, USB, romfs, IRQs, timer, display, buzzer, CMCU, RTC, input, network,
CF, and audio before services and scheduling. Thus romfs NAND reads precede
timer initialization; their timeout must not depend on `time_us()` yet.

CE RAM boot inherits an operational bus, power state, UART and potentially
live LCD. Its trampoline quiesces selected hardware and the kernel reclaims it;
it also keeps romfs in RAM, bypassing NAND-backed executable faults. Hardware
success through GlazeBoot therefore does not exercise the NAND SPL or establish
cold-boot power defaults. See [GLAZEBOOT.md](GLAZEBOOT.md).

## Initial dependency matrix

“Missing” below describes absent Glaze setup; it does not prove it must be
duplicated if the ROM supplies it. Each open inheritance question stays open.
P0 blocks boot/flash confidence; P1 blocks usable hardware; P2 is peripheral
verification. Emulator observations are direct source findings, not silicon facts.

| ID / priority | Routine or dependency; owner and evidence | Glaze state | Emulator behavior that can conceal it | Next verification / fix |
| --- | --- | --- | --- | --- |
| H01 / P0 | BCU/SDRAM setup, ROM `9FC005A0`, stock SPL `80F025A0`; halfword accesses listed above. Direct. | **ROM-provided** basic setup; no Glaze reprogramming. Exact handoff timing values open. | `E:gxemul/src/devices/dev_vr41xx.c` BCU/SDRAM handlers store bytes; RAM works without enforcing refresh, timing or bank configuration. | Record executed ROM table selection and final values at SPL entry; compare against board documentation. Preserve working ROM setup unless a demonstrated difference requires a change. |
| H02 / P0 | CPU cache/config and executable visibility; ROM tag init, stock SPL `80F02404` switches to KSEG1 before its setup. Direct. | **Partial:** kernel workaround exists; SPL executes C and stack through KSEG0 before that workaround. Kernel copied via KSEG1, then entered via KSEG0. | Host-backed RAM and CPU translation are insufficient evidence of real cache coherency or VR4131 CACHE errata. | Prove ROM cache state and SPL cache safety; verify SPL text/stack and loaded kernel visibility on silicon. Consider moving SPL execution/stack uncached before C; do not label kernel workaround missing. |
| H03 / P0 | Early companion SIU power, clock, divisor, line format and cable transceiver. `[console.c](../kernel/console.c)` documents CE COM1 ownership. | **Missing explicit cold UART setup:** SPL polls LSR bit `0x20` at `AA008694` forever and writes bytes at `AA008680`; kernel bounds its polls. ROM inheritance unproven. Stowaway setup occurs much later. | Companion SIU is a generic ns16550 (`addr_mult=4`); `E:gxemul/src/devices/dev_ns16550.c` seeds 9600 8N1 and services TX without a physical transceiver. Companion clock block is mostly storage. | Identify ROM/stock serial enable routine and handoff values. Initialize or explicitly validate UART before output; bound SPL polls so unavailable serial cannot stop boot. Test output without CE or dock initialization. |
| H04 / P0 | NAND unit clock `0A001134`, framing `0A00A400/A408/A40C/A410`, phase/ACK and stream state. All framing writes are 32-bit; command/address bytes 8-bit. | **Partial:** kernel `nand.c:select()` enables clock and four control words; SPL omits these, writes ACK rather than kernel's read ACK, omits phase-close/deselect and engine reset. ROM inheritance unproven. | `E:src/hw/nand.c:nand_write()` starts a stream on MODE 5 and sets ready immediately without requiring all those enable words or clock. `be300_devices.c:cmu_write()` stores NAND clock without enforcing it. | Compare raw stock ROM/SPL NAND instruction sequences with both Glaze paths. Establish the handoff contract, then make SPL independently frame reads and bound readiness waits. Test with clock/control words cleared and stale engine state. |
| H05 / P0 | NAND page completion and error reporting. Stock kernel reference range `80F0A658..80F0AE80` is documented in Glaze `nand.c`; physical page is 512+16 bytes. | **Partial:** SPL drains 512+8 bytes; kernel drains 512+16 and resets MODE. SPL readiness wait is infinite; kernel logs timeout then continues without returning read failure. | MODE 5's comment says 520 bytes, but its actual `stream_limit` is `NAND_PAGE_RAW-col` (528 at column zero); a subsequent MODE 5 restarts cursor. No physical transfer latency. | Resolve mode/trailer contract from stock instructions or hardware capture. Exercise consecutive reads and page/block boundaries. Propagate failure before jumping to corrupted code; determine exact drain/reset requirements rather than blindly changing 8 to 16. |
| H06 / P1 | GIU line 0 direction, polarity, edge/level and companion source routing; registers `0F000140`, `150/154/158` (16-bit). | **Partial:** `irq_init()` masks ICU/GIU, clears pending and enables line 0, but does not program direction/type/polarity/hold selection. Companion source controls mostly use read/modify/write. | `dev_vr41xx.c` latches type/polarity/hold registers; GIU callback routing does not model all pin configuration, and CPU IP2 uses an edge-delivery workaround. | Trace stock OAL and ROM for all line-0 prerequisites, then explicitly establish required routing. Test level assertion, deassertion and a held foreign source, not just successful tick polling. |
| H07 / P1 | Touch PIU and key scan power/clock; source 9, `0A000300` control halfword, interval/stable/mask. | **Implemented PIU sequencing:** `input_init()` writes interval `05DC`, stable `00C8`, interrupt mask and scan control. **Open:** upstream power/gate and keyboard scan setup. | `E:src/hw/piu.c` models sequencer timing but does not establish the whole companion power tree; `be300_devices.c` seeds core offset 0 with `2` from a hardware snapshot. | Identify whether core PIUPWR and peripheral scan controls are ROM defaults or stock SPL/OAL work. Test repeated physical touch release/repress and buttons with scripted injection disabled. |
| H08 / P1 | LCD/CMM/backlight/panel sequence, stock SPL `80F034A0`, `80F038DC`, OAL `80078F38`. | **Implemented:** `display_init(false)` programs clocks `1100/110C`, shared gate `1128`, backlight and panel sequencing, bit-serial contrast, and delays (1/1/20 ms). RAM boot alone may inherit validated live state. | `E:src/hw/lcd.c` models owned gates and display state; this is stronger coverage than generic latch registers. Panel electrical behavior still requires hardware evidence. | Verify a NAND cold boot paints without CE; preserve shared buzzer gate/config. Do not add duplicate LCD init based on old post-CE assumptions. |
| H09 / P0 | PMU HALTimer and Casio MCU watchdog/power protocol, `0F0000C2` halfword and `0A007800` block word accesses. | **Implemented basic servicing:** ROM and `kmain` acknowledge HALTimer; `cmcu_init/tick` configure `7810=10000`, reload `7814=96` every 5 s, send `780C=53`, `7808=3A` at 10 Hz. **Open:** cold MCU power commands beyond these. | PMU has a real timer model (`E:src/hw/pmu.c`). ScCmcu reads in `be300_devices.c:scmcu_read()` always return zero, simulating instant completion; no MCU watchdog enforcement. | Decode ROM MIPS16 MCU initialization and stock power driver. Boot longer than 150 s and compare missing/active keepalive behavior; prove cold boot does not require extra commands before peripheral access. |
| H10 / P1 | RTC oscillator, Count calibration and timer tick. ROM `9FC0064C` already uses RTC. | **Implemented** elapsed reads, calibration and Count/Compare scheduling; no oscillator bring-up is established as necessary after ROM. `measure()` bounds initial tick wait but its subsequent 100 ms wait is unbounded. | RTC advances from emulator time; host-time option can supply a date. Clock straps/Count divisor are modeled, not electrical oscillator startup. | Use zero-epoch cold boot without host-time override; compare measured rates and scheduler. Fault-test RTC stopping during measurement. Fix stale comment claiming Count is half PClock; current emulator evidence documents PClock/16. |
| H11 / P1 | Audio codec, DMA and companion wake, stock wavedev `01912478`, `019113CC`. | **Implemented** codec wake `1118=1`, idle/format and `D007`, buffers, start-time wake `1104`, power, volume and IRQ control. Exact cold codec prerequisite state open. | `be300_devices.c` seeds codec ready, format, power, DMA and audio wake words from post-CE captures; `E:src/hw/aiu.c` models audio behavior. | Start with documented reset state, not the audio snapshot. Prove ready can transition after cold power enable, bounded timeout/retry works, DMA buffers are coherent and physical PCM plays. |
| H12 / P1 | PCMCIA bus timing and socket supply GPIO; raw stock SPL `80F03248/3290/32C4`, called from `80F0304C`. | **Missing explicit baseline:** stock writes gate `0A00111C=1`, timing `A010=1073`, `A014=1061`, `A018=10B1` (32-bit); ORs GPIO5 into `0F000140/144` (16-bit), waits using helper argument `136`. Glaze socket setup does not reproduce this. ROM ownership and actual delay units open. | Companion gate is seeded `111C=1`; GPIO/timing writes are latches rather than a full bus/power model. | Determine which settings survive ROM and whether GPIO5 is socket power on this board. Validate timing values and helper units. Explicitly establish required baseline before reading CIS or touching ATA; do not assign guessed milliseconds to `136`. |
| H13 / P2 | Socket reset/CIS/COR and ATA/network startup. | **Partial:** hot insertion has `socket_power_up()` (300 ms reset, 20 ms settle), CIS/config and retries. Boot-time `probe()` can take the already-configured storage path, relying on inherited socket/COR state. | Glaze source notes emulator cards ignore COR; powered/decoded access therefore does not prove cold physical card configuration. | Compare cold card present versus insertion after boot; establish same required configuration for both, including ATA 8-bit mode, network COR and card readiness. |
| H14 / P2 | USB function power/reset/DMA/descriptors. | **Implemented:** `usb_init()` enables `1114`, pulses unit reset and sets descriptors, DMA and polling. Uses loop delay before timer exists. | USB functional model plus preseeded `1114=1`; common power-tree completeness is not verified. | Test cold USB enumeration with gate initially off, DMA coherency, detach/reattach and unavailable unit. Check loop delay meets silicon minimum at ROM-selected clock. |
| H15 / P2 | Piezo PWM shared CMM gate/config and channel. | **Implemented:** `buzzer_init()` stops channel; tone path sets shared gate, config, period/duty/start. Uses `09A0/9A4/9A8`, not backlight channel. | LCD model owns shared gate/PWM and drives host sound. | Verify tone with display off/on and preserve shared config/gate; this is not a missing driver. |
| H16 / P0 | Kernel exception vectors, TLB and CPU interrupt masks. | **Implemented** kernel trap/MM setup; SPL explicitly clears Status/Cause. Stock-like uncached early setup still needs H02 verification. | Emulator provides CPU/MMU execution, but cannot certify silicon hazards, cache ordering or interrupt electrical behavior. | Review vector copy synchronization and CP0 hazard spacing against the VR4131 reference; exercise timer/IP2 after a true ROM boot and with dirty inherited cache/TLB state on RAM handoff. |

## NAND image readiness (separate from initialization)

| ID | Finding and evidence | Required closure before a flash procedure |
| --- | --- | --- |
| N01 | [mk_nand.py](../tools/mk_nand.py) emits exactly 16 MiB of page data, a partition table, a B000FF SPL and fixed kernel/romfs/filesystem offsets. `E:src/hw/nand.c:nand_stream_oob_byte()` synthesizes spare tags from image coverage. | Specify physical-page/OOB encoding and who writes it. Prove the masked ROM recognizes the actual partition/SPL placement and spare tags; do not feed a data-only emulator image to an unspecified raw programmer. |
| N02 | Real CE data uses logical-block tags/remapping. `flash.c` builds a CE map and decodes RS parity for `/NAND`, but normal SPL kernel reads and `romfs.c` use fixed absolute page numbers without that map. | Choose and document physical placement/lookup for immutable boot content, including factory bad blocks. Test a bad block inside SPL, kernel and romfs independently. CE mapping support in `/NAND` does not cover these readers. |
| N03 | Kernel/SPL boot reads do not perform ECC correction. The CE read path's `nand_ecc.c` and filesystem BCH checkpoint parity are separate mechanisms. | Define boot-content ECC/error detection and recovery across the entire chain, including the ROM's own expected ECC/spare layout. Corruption must prevent transfer to an invalid kernel. |
| N04 | `servers/fs/ftl.c` tracks runtime failures, but initialization explicitly omits factory spare-area bad-block marks. README also calls this out. | Import factory bad marks before allocating/erasing filesystem blocks, including treatment of bad BBT storage blocks. Never erase the evidence before importing it. |
| N05 | `mk_nand.py` uses `FFFFFFFF` record checksums and emulator-oriented layout conventions; existing ROM handoff notes confirm stock record packing but retain MIPS16 loader uncertainties. | Verify checksum semantics, record boundaries/entry and SPL reserved area against the raw ROM. Confirm actual output's ranges do not overlap, and distinguish proven container structure from emulator acceptance. |

## Prioritized follow-up and verification

These are proposed follow-up changes, not changes made by this audit. No public
API was changed. A read-status interface would be needed if NAND failures are
to propagate safely from SPL/kernel readers rather than being logged and ignored.

1. **Close the ROM handoff boundary first.** Capture every MMIO read/write with
   PC, width, value and boot stage from reset through first Glaze SPL instruction,
   particularly the MIPS16 blocks. Decode unresolved blocks against raw ROM;
   compare with hardware manuals/captures. Record exact bus, clock, GPIO, NAND,
   serial and MCU state. This determines which “missing” routines are ROM-owned.
2. **Make early SPL loading dependable (H02–H05).** Establish uncached/cache-safe
   execution, bounded optional serial, NAND enable/framing/read completion and
   fatal read handling. Cross-check stock hardware sequences before implementing
   stricter emulation; do not merely enforce Glaze's current sequence as truth.
3. **Close board power and interrupts (H06/H07/H09/H12/H13).** Supply confirmed
   GPIO, gate, socket timing and interrupt prerequisites. Keep already implemented
   panel/audio/USB routines and validate them from cold state.
4. **Add emulator checks where behavior is known.** Distinguish true reset
   constants from CE snapshots; enforce clock/reset dependencies, NAND control
   state and busy transitions, GPIO power/interrupt behavior and card decoding.
   Model ScCmcu timing/watchdog only to the extent hardware evidence supports it.
   Do not zero strap-backed identity registers to simulate a cold boot.
5. **Resolve N01–N05 before defining a flashing procedure.** Hardware tests at this
   stage should be non-destructive RAM-loaded probes; a CE handoff validates
   transaction behavior but cannot certify ROM-to-SPL cold boot. Any later NAND
   write test needs a separate concrete recovery/backup and image-placement plan.

Acceptance scenarios for the follow-up implementation:

- Cold ROM boot without CF, with storage present, and with a network card present;
  repeat with insert/remove after boot. Capture real touch/buttons, not just
  scripted events, and exercise the companion interrupt path.
- Paired mutation tests on disposable emulator images/models: remove one proven
  required initialization write and require its dependent operation to fail;
  restore it and require success. Gate-off reads must not silently succeed.
- Consecutive NAND reads across page/block boundaries; unavailable serial; NAND
  never ready; RTC stops during calibration; stale DMA/stream state; pending
  foreign interrupts. Fail with bounded diagnostics instead of hanging or using
  corrupted data. Early checks must work before timer initialization.
- Bit errors and factory bad blocks in boot data and filesystem; verify correction,
  explicit rejection or defined recovery. Round-trip physical OOB/tag encoding
  against real captures rather than synthetic emulator tags.
- Run beyond 150 seconds with MCU keepalive enabled; separately demonstrate its
  modeled failure when omitted. Exercise panel/buzzer power sharing, PCM and USB.
- Repeat CE RAM handoff to preserve existing functionality, including dirty cache,
  active devices, and RAM romfs. Report cold boot and RAM boot results separately.

## Initial audit validation and remaining limits

Performed: direct source comparison of boot, kernel startup, hardware drivers,
romfs/flash/FTL, packer, emulator reset initialization and relevant MMIO handlers;
raw little-endian MIPS32 decoding with Capstone 5.0.7 of the ROM timing/clock/PMU
routines and stock SPL entry, timing, socket/GPIO and backlight routines. The
specific stock instruction effects above were checked against the binary,
not inferred from post-CE register values. Inspection also established the
emulator cold path starts at `BFC00000`; that does not remove its device-model
shortcuts.

No new hardware runs, emulator traces, builds, firmware writes or fault-injection
tests were performed for this documentation-only change. Existing tests and
successful boot captures are not presented as validation of new cold-boot
behavior. The matrix covers every subsystem initialized by current `kmain`, plus
its early boot dependencies; it is **not proof that every board initialization
routine has been found**. Exhaustiveness depends on closing the MIPS16 handoff,
stock OAL initialization call graph, undocumented power protocol and physical
reset-state evidence. These are explicit outstanding audit items, not claims
that unknown writes can safely be omitted.


## Implementation follow-up

The confirmed Glaze initialization gaps now have these changes:

- **H02:** SPL code switches to KSEG1 before C; its stack uses KSEG1, ROM cache
  lines are flushed by index before copying the kernel, BSS is explicitly
  cleared, and kernel entry is through its uncached alias. The kernel retains
  its existing VR4131 Config erratum workaround. This removes early reliance on
  cached SPL execution/stack and stale kernel instruction lines.
- **H03:** unavailable serial is bounded and disabled after the first timeout.
  Boot no longer depends on a functioning debug cable/UART. Electrical UART
  clock/transceiver setup remains an open ownership question; no guessed gate
  values were added.
- **H04/H05:** SPL enables the NAND unit clock and PIO control words, reclaims
  DMA and resets the chip/engine, reads ACK, closes phases, drains 528 bytes,
  resets mode and deselects after each page. This uses the existing kernel's
  hardware-tested PIO sequence. SPL and kernel readiness timeouts stop execution
  before consuming stale page data. Kernel reset now enables the clock before
  touching the DMA/engine. No ECC or physical bad-block remapping was invented.
- **H06/H07:** GPIO0 is explicitly configured as an input with active-high level
  interrupt routing, matching the CE capture and Linux vr41xx GIU semantics.
  Hold is cleared for a level input. Other pins' configuration is preserved.
  Touch source 9 is masked during IRQ setup and enabled after PIU programming;
  it no longer depends on the emulator's seeded source-enable word.
- **H10:** both RTC calibration waits and stable-counter reads are bounded,
  including a counter stopping after its first transition. Failure falls back
  to the existing Count estimate. The Count divisor comment is corrected.
- **H12/H13:** before cold-boot card detection, Glaze programs the stock SPL's
  socket clock, bus timing and GPIO5 writes. A present card receives the same
  power/reset, CIS and configuration sequence as hot insertion, before ATA or
  NIC probing. The presence latch is set before IDENTIFY. A NIC already loaded
  by this path is not loaded twice. Normal CE RAM handoff keeps its live socket.

The socket routine has an internal `force` parameter for a hardware diagnostic.
`./build.sh glazeboot COLD_SOCKET_TEST=1` enables it after RAM handoff, with
romfs/root still in RAM and CE NAND read-only. Only the always-rebuilt kernel
entry uses the build flag, so reverting the option cannot retain a stale test
setting in the socket object. Build normally without the flag afterwards.

`make -C host boot-init-test` executes production SPL and kernel NAND readers
against a host MMIO model that rejects gated transfers, DMA ownership, incomplete
spare drains and ACK writes; it covers page/block/address boundaries, missing
serial, and fail-stop timeouts. It also tests production RTC calibration with a
stopped/torn counter and wraparound, plus IRQ setup from deliberately wrong pin
configuration while preserving unrelated bits. These tests use ASan/UBSan.
`net-driver-test` covers dispatch after the early NIC initialization change.

Hardware verification uses the attached BE-300, not a NAND flash:

- CE recognized the SanDisk 1 GB CF card. USB upload/read-back/execution check and
  GlazeBoot staging used `\Storage Card\GlazeUSB`.
- Updated Glaze booted through CE and reconnected after USB RAM reload. Logs show
  the SanDisk SDCFJ-1024 (2,001,888 sectors), a mounted 977 MB card filesystem,
  CE NAND mounted read-only, and SpringBoard ready. A USB screenshot confirmed
  the rendered home screen.
- The user confirmed two separate physical screen taps and a hardware button
  responded after explicit interrupt/touch initialization.
- The forced cold socket diagnostic also mounted the physical card and reached
  SpringBoard. Its first run revealed the missing software presence latch above;
  the corrected run initialized and mounted it without those retries. Logs are
  recorded under ignored `out/nand-init-check`.

Emulator checks boot disposable copies of the NAND image from ROM reset without
host-time seeding, both without a card and with storage or NE2000. All reached
SpringBoard; storage mounted and NE2000 obtained DHCP. The SPL-scattered RAM boot
also reached SpringBoard. Logs live under `out/nand-init-check/{cold,card,net,ram}`.

Still open: exact MIPS16 ROM handoff ownership, UART electrical enable, broader
companion power/reset modeling, MCU protocol and physical boot image/ECC/factory
bad-block policy (N01–N05). The sibling emulator's permissive clock/reset models
were not changed by this patch; the new host MMIO tests enforce the Glaze loader
prerequisites independently. RAM-boot hardware success does not prove that the
masked ROM will load a physically programmed Glaze image. No NAND flashing was
performed.


## Blocker follow-up

The shared SPL/romfs boot reader now maps tagged logical blocks and checks page
ECC before consuming content. The hardened decoder verifies all six syndromes
and repairs packed parity symbols. Independent fault tests and a mandatory-ECC
scan of the physical BE-300 passed. ROM MIPS16 decoding also resolved the record
walker/checksum behavior and confirmed several clock/socket settings are already
ROM-owned. See [NAND_ROM_HANDOFF.md](NAND_ROM_HANDOFF.md) for exact addresses,
reproduction commands, test evidence and the remaining physical-image/erase
blockers. No physical NAND writes were performed.


## Power-on findings (2026-10-30)

Earlier "cold" hardware tests were Power+Reset into the CF recovery payload.
The companion keeps its clock gates and interrupt setup across that reset, so
Windows CE's state leaked into every one of them. A true power-on (battery out,
card in) is the closest non-destructive stand-in for a NAND boot, and it
exposed the gaps below. No NAND was written.

**Method.** `HWDUMP=1` kernels ([hwdump.c](../kernel/hwdump.c)) snapshot the
VR4131 units and companion windows at kernel entry and after driver setup.
SpringBoard saves them to `/Card/HWDUMPnn.BIN` and `/Library/Caches/HWDUMP.BIN`
and posts lock-screen summaries. `BOOT_BEEP=1` adds piezo checkpoints (the
recovery stub always beeps low on entry and before the kernel jump) for boots
with no LCD, serial or USB. Once USB came up, the out-of-band EP0 request
gained an `HWDUMP=1`-only word read/write: `glaze-usb peek`, `peek-dump` and
`poke`. That allowed live bisection on the running cold device.
`tools/hwdump_diff.py` compares cold, warm (GlazeBoot) and BEDiag values.

**Bus stalls.** Any read *or write* of a companion unit whose clock gate is
off hangs the bus for good: a black screen, no response, and recovery needs
the card out plus a battery pull. Bounded polls don't help. At power-on the
ROM's CF path leaves only gate `111C` on. Three hangs came from this:

| Where | Access | Fix |
| --- | --- | --- |
| Recovery stub green marker, kmain boot-trace boxes | Framebuffer `0A200000`, behind `1100`/`110C` | `gz_boot_trace_live()` gates every early trace write |
| Entry snapshot (first `HWDUMP` build) | USB/LCD/PIU windows before their gates | Entry snapshot limited to ROM-live units |
| First console write | SIU `0A008680` | `companion_gates_on()` in kmain |

**Gaps fixed, each verified on the device:**

- *Gates and GPIO* (`companion_gates_on`): `1104 1108 1120 112C 1138 113C`,
  `1B24=0 1B20=1 1B2C=1`, `A0DC=FF`, and GPIO4 (low) and GPIO6 (high) as
  outputs, which is CE's steady state. Proven: the SIU stall cleared once
  `1108/112C/1138/113C` were set, and USB enumerated only after the GPIO4/6 and
  `1B2C` round. Each of these writes is not yet individually proven.
- *Card detect*: `0A001000` bits 2-3 read 4 only after CE has configured
  the card (0 after power-on with a card, 8 with none). Presence now comes
  from the socket's card-detect inputs, `A008` bits 1-2 (`2F9`/`2B9` with a
  card, `3FF` without). Cold boots now mount, format and write the card.
- *Interrupt delivery*: `0A000008` bit 0 drives GIRQ0 out to GIU line 0. It
  is 0 after power-on, so no companion interrupt ever reached the CPU (the
  SYSINT1 GIU bit appeared when it was poked). Each source word also needs bit 2,
  which CE sets on every source it uses. Without it touch's pending latch
  (bit 7) never cleared, and enabling the output alone stormed. `irq_init`
  sets both, rewriting the masked source words to clear stale latches.
- *Touch*: `0A0003C8` must be 3 before the PIU sequencer starts (CE leaves 3,
  power-on leaves 0). Set on a running PIU it changes nothing until the next
  restart. Bisected live: the `0A000200` unit, `A060-A07C`, the codec command
  and PIU `0x310` were not needed.

**Still open:** the power-on boot with all fixes but no live pokes needs a
final confirmation run. Also open: which gate clocks which unit, the
VR4131 CMU (`0F000060` reads 0 cold vs `902` under CE, and nothing has needed it
yet), the PMU/RTC differences, and the 150 s watchdog on a power-on boot. The
NAND SPL wrote to the SIU before anything else, which would stall a power-on
NAND boot the same way. It now samples the four candidate gates first and
stays silent unless they are all on (`spl-io` covers both cases), so a NAND
power-on boot prints nothing until the kernel's console.
A ktest NAND image overflows the romfs slot (4 KiB over at `bad936b`, 16 KiB
with the working tree), so ktest ran from a CF recovery boot instead (ALL PASS).

## Boot beeps

A NAND boot has no LCD (its gates are off until `display_init`), no serial
(the SIU is gated) and no USB until the kernel's driver is up. The piezo is
the only signal, and it needs only gate `1128`, which the SPL and kernel set
themselves and then put back. Beeps are timed by CP0 Count (near 10 MHz).

| Who | Sound | Meaning |
| --- | --- | --- |
| SPL, `BOOT_BEEP=1` | 1 short 1 kHz | ROM started the SPL |
| | 2 short 1 kHz | NAND reset (command phases only) |
| | 3 short 1 kHz | companion gates on (`1104 1108 1120 112C 1138 113C`) |
| | 4 short 1 kHz | NAND DMA released (`1CF4=1`, `1CC0=0`), engine reset again |
| | 5 short 1 kHz | boot block map read (first stream-port page data) |
| | 6 short 1 kHz | kernel loaded, jumping |
| SPL, every build | 1 s 500 Hz, then *n* short 1 kHz, three times | fatal stop: 1 NAND timeout, 2 missing or uncorrectable boot page, 3 ambiguous boot block map |
| Kernel, `BOOT_BEEP=1` | 1–6 short 2 kHz | `kmain` entry, after the entry snapshot, console up, `usb_init`, `irq_init`, `timer_init` |
| CF recovery stub | 400 ms 1 kHz | entry, and before the kernel jump |

So a `BOOT_BEEP=1` NAND boot should sound 1 to 6 low then 1 to 6 high. The
last group heard places a hang.

The first NAND boot (2026-10-30) beeped 1 and went silent, with the old
three-checkpoint SPL. Dropping its writes to the NAND DMA words `1CC0/1CF4`
changed nothing. Later builds got further: through the gates, through the DMA
word and engine reset, and then went silent at the first page read every time.
Casio's own SPL, extracted from the device's CE backup and traced in the
emulator, gave the two missing pieces:

- **Page reads are split.** The ROM (`9FC01A98..1AAE`) and Casio's SPL
  (`80F0ADFC..AE28`) read 520 bytes in MODE 5, then switch to MODE 4 for the
  last 8 spare bytes, just as programming splits MODE 7/6. Glaze read all 528
  bytes in MODE 5, and the 131st word never came. The SPL and the kernel's
  `nand_read_page`/`nand_read_raw` now split the same way. The kernel's
  unsplit raw reads had worked in recovery boots, so something there differs;
  that is still open.
- **Gates before the DMA word.** Casio's SPL turns on `1128`, `1100` and `110C`
  (for its LCD splash) before it writes `1CC0 = 0`, and it never writes
  `1CF4`. The SPL now does the same, plus the kernel's gate set. This was
  never shown to be needed. Every failing build hung at the first page read,
  including the first two (whose silence after checkpoint 1 covered both the
  DMA writes and the scan). Only the split read changed between the last
  failing build and the one that booted. The gates match Casio's SPL and do no
  harm, so they stay.

With both, the sixth build booted from NAND to SpringBoard on 2026-10-31,
with USB working. Builds with `BOOT_BEEP=1` also play seven 3 kHz ticks
through the first page read (selected, READ0, address, ready, first word,
520 bytes, last spare bytes), between SPL checkpoints 4 and 5.

The emulator then modelled neither, so it booted every failing build. Since
be300 `docs/POWER_ON_STATE.md` it models the 520-byte burst after the ROM's
NAND boot, so the failing builds stall at their first page read
(`[BUS] stall: read of NAND stream`). It doesn't model the gates, because
nothing shows they matter. The SPL's fatal stops never jump to a partly
loaded kernel. The emulator plays the same tones with `--audio`, and
`--log-mmio` shows the `9A0`/`9A4` writes. A physical candidate with a kernel
page corrupted past its ECC gives checkpoints 1 and 2 and then error 2.
`tests/spl_io.c` checks the error pattern at power-on gate state.
