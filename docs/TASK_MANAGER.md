# Task Manager

Task Manager 1.0 (`packages/taskmanager`) is an App Store package. Open App
Store, find Task Manager, and install it on Flash or Card. CPU and task
monitoring require an OS image built with `SYS_TASK_SNAPSHOT` and
`SYS_TASK_STOP`; use the current `out/glaze.bin` for those features. On older
Glaze builds, the same package shows memory and uptime and explains the
required update on the Tasks screen.

## Screens and measurements

- Performance samples once per second with a rolling 60-sample CPU and memory
  history. CPU is the fraction of guest monotonic time scheduled outside the
  idle thread. Kernel execution is charged to the process running at the time.
  This measures guest activity, not the host emulator's CPU usage.
- Memory used is all SDRAM minus allocator free pages, including the kernel
  and reserved memory. Per-task mapped RAM counts valid owned, image and
  shared-memory pages, excluding device and romfs resource mappings. Shared
  pages appear in each process mapping them; task totals must not be summed
  as physical usage. Page tables and kernel stacks are excluded from task RAM.
- Tasks sorts by interval CPU, with PID as a stable tie-breaker. Tap a row for
  PID, parent PID, threads, runnable threads, mapped RAM, interval CPU and
  cumulative CPU time. The first sample displays `Sampling...`.
- Pause freezes measurements and history. Live resumes sampling; its first
  interval covers the pause. Selections use generation-bearing PIDs rather
  than table positions, so an exited/replaced task cannot redirect an End
  Task confirmation to another process.

Glaze runs one foreground app and terminates it on Home. The list normally
contains Task Manager and system services. End Task is offered only for
another managed app when one is present. Confirmation ends it immediately and
can discard unsaved work. The kernel protects Task Manager itself and system
services, including when calls bypass the UI.

## Kernel interface

Syscalls are appended to `glaze/abi.h`, preserving existing numbers and the
`gz_sysinfo` layout. `gz_task_snapshot` passes the version-1 structure size;
unknown sizes and invalid output buffers fail with `GZ_EINVAL`. Snapshots are
serialized with scheduling and contain up to 32 live processes, a timestamp,
cumulative non-idle time, physical page counts and per-task counters.

Scheduler accounting includes the current unfinished slice and accumulates
process CPU across all threads, including exited threads. Sampling and context
switches advance the same accounting baseline to avoid double counting.

Installed ELF images receive `PROC_APP` from the kernel. SpringBoard marks
built-in foreground apps with `GZ_SPAWN_APP`; only privileged callers may set
that spawn flag. `SYS_TASK_STOP` accepts only a process whose flags are exactly
`PROC_APP`, rejects the caller, and checks the full PID generation. It grants
no framebuffer, input, storage-driver, network-driver or audio-driver access.
The existing `gz_kill` permission policy is unchanged.

## Validation

```sh
./build.sh
./build.sh store
make -C host taskmanager-test store-catalog-test
python3 tests/store_publisher.py
python3 tools/publish_appstore.py --dry-run
```

`taskmanager-test` runs production sampling and UI logic with ASan/UBSan. It
covers interval CPU, history rollover, PID reuse/reordering, invalid time
intervals, confirmation/cancellation, protected/self tasks and old kernels.
It renders screens under `out/host/taskmanager-*.bmp`.

Kernel `ktest` checks snapshots, invalid output buffers/sizes, real CPU
accounting, managed app flags, and permissions from an unprivileged peer. That
peer can end a managed app but cannot end itself or a system process, grant
managed-app flags, or act on an already-ended PID.

For offline installation and UI checks, put only this package on a card:

```sh
tools/mkcfimg.sh out/taskmanager-cf.img 16 16 out/pkg/taskmanager.gzp
CF=out/taskmanager-cf.img tests/run.sh tests/t_taskmanager.txt 90
```

For HTTPS store installation, build with
`INPUT_SCRIPT=tests/t_appstore_taskmanager.txt`, boot a fresh copy with
`--rtc-host-time --ne2000`, and allow at least 75 guest seconds. Ship the OS
with the default HTTPS store URL and no input script.

Regenerate only this app's original vector-based icon:

```sh
swift tools/mkassets.swift assets/gen assets/fonts userdata/Pictures --taskmanager-only
```
