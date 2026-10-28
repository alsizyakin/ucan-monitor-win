# ucan-monitor-win

The Windows build of uCAN Monitor (github.com/OlegAushev/ucan-monitor, branch
`develop`) with a candleLight (gs_usb) USB-CAN adapter driven over libusb.
Upstream is not ours: its developer works on Linux and knows nothing of this
repository. Colleagues download the releases; the owner (alsizyakin) tests.

## Rules

- **Never change upstream or send it changes.** The owner does not want to
  take the developer's time. What Windows needs lives here: `win/include`
  shadows upstream headers; `win/patches/*.patch` and `win/patches/renames`
  fix upstream code in the local copy only (`win/patches/apply.sh`).
- **Windows 10 and 11 alike** (both verified 2026-10-15). Nothing may rely on
  Windows 11 behaviour. A manual CI run can build on `windows-2022`, which
  treats DOS device names like Windows 10.
- **A push to `main` publishes a release** that colleagues get by
  `releases/latest/download/ucan-monitor-win.zip`, unless only `*.md` or
  `docs/` changed. Untested changes go to a branch first; build one with
  `gh workflow run windows.yml --ref <branch>` (an artifact, no release).
- Replies to the owner in Russian; code, comments and commits in English, in
  the style of the existing ones.

## Build and test

- Linux: `sh win/checkout-upstream.sh` (`--latest` for the tip of develop),
  `sh win/patches/apply.sh`, `cmake -S . -B build -G Ninja`,
  `cmake --build build`, `ctest --test-dir build`. Programs land in
  `build/bin`, next to `build/assets`.
- The adapter on Linux needs root; the owner runs `sudo` commands and pastes
  the output (no udev rule installed as of 2026-10-17). Opening the adapter
  takes it from the kernel's gs_usb driver: `can0` comes back DOWN after.
- `candle-test`: `-l` lists adapters; `-m loopback` is a self-test that sends
  nothing to the bus; `-c -p 1` a load self-test with numbered frames;
  error frames are printed in words. Never use `-s` on a bus with a device
  without the owner agreeing to the frame.
- GUI checks without a screen: `xvfb-run` with
  `env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11`, or GLFW opens the window on
  the owner's Wayland desktop. The Windows build runs under wine with a
  `WINEPREFIX` in the session scratchpad.
- Drive firmware sources, to read what a device checks:
  `~/Git/MCU/<device>` (e.g. `adpt-bike-inverter`).

## Open work (2026-10-17)

**adpt-bike on Windows: "Некорректный кадр VCU" warnings and drive stops**,
seen by the owner with release `win-17`. From the firmware
(`~/Git/MCU/adpt-bike-inverter`): RPDO1/RPDO2 carry a 2-bit counter that must
grow by one (emblib `node_link::counter_ok`), so a lost frame makes the next
one invalid; no valid RPDO1 for 300 ms (`settings/canopen.hpp`, `to_critical`
0) is a critical link loss and the drive stops. The client heartbeat is not
checked. Upstream's RPDO service ignores the `send()` status after advancing
the counter, so every frame our `send()` drops is a counter gap. In `win-17`
`send()` waited up to 50 ms for its USB transfer and dropped the frame on a
timeout: the suspected cause, not reproduced here.

**Branch `async-send`** (`d35c2f8`, `2a7d876`; not on `main`):

- `send()` queues and returns; 8 USB transfers in flight, order kept; a frame
  is refused only with 256 waiting;
- `can::Socket` logs `CAN, last 10 s: …` for any 10 s with a failed or refused
  frame, a send of 20 ms or more, or the uCANopen thread not reading for 50 ms
  or more, and `CAN in this connection: …` on disconnect;
- `candle-test` load self-test; manual workflow runs always release.

Built by Actions run 38748096606; the archive is that run's artifact and
`dist/ucan-monitor-win-test.zip`. The local `build/` and `dist/ucan-monitor/`
are this branch.

**The owner will bring:**

1. the output of
   `candle-test -m loopback -b 500000 -s 123#0000000000000000 -c -p 1 -t 20`
   on Windows, maybe on Linux too. Expected: `lost 0`, `out of order 0`,
   `USB: … failed 0, … dropped 0`. Compare the longest send and the round trip
   between the two;
2. `bin\log.txt` after 10-15 minutes with adpt-bike on Windows, and whether
   the warnings and stops still happen.

**2026-10-19: the async-send build did not help.** The drive still raised
"Некорректный кадр VCU" then "Потеря связи с VCU" (they clear, work goes on);
on the same bench upstream's Linux build is clean. The log of that run
(`~/Git/PC/ucan-monitor/logs/log.txt`): sent 7505, failed 0, dropped 0,
received 27039, longest pause 36.3 ms, no `CAN, last 10 s` line at all. So
every frame reached the adapter over USB; they go missing after it. The
RPDO layouts of upstream and firmware match bit for bit and the reserved
fields are zero, so an invalid frame is a counter out of sequence. Suspects:
the adapter (firmware 2) dropping frames it took (the Linux driver numbers
frames and keeps at most 10 un-echoed; we sent echo_id 0 and ignored
echoes), or the bus (an error at the end of a frame can make a receiver take
it twice).

Added on the branch (`c3dd9ab`, `8fc4f9e`; Actions run 39277266608):
running echo_id, echoes matched, a frame not echoed within 1 s counted as
`unconfirmed by the adapter`, bus error frames counted, both in the log.txt
lines; `bin/cantrace.on` turns on a CAN trace `bin/cantrace-*.csv`
(S/E/M/F/D/R/X); `win/tools/analyze-trace.py <trace> --node 1` checks the
RPDO counters as sent and as confirmed and puts each drive warning next to
what happened in the second before.

**2026-10-20, trace build on Windows** (logs `~/Git/PC/ucan-monitor/logs/log (2).txt`,
`log (3).txt`): the drive now loses the VCU link all the time. Each 10 s:
~1120 sent, `unconfirmed by the adapter` 9-12, at times ~100; failed 0,
dropped 0, bus errors 0, longest send 1.6 ms, longest pause 0.8 ms; received
~3070 (as expected). So the adapter takes frames over USB and loses them;
the pool is not exhausted (sends would wait). candleLight firmware always
reports "firmware 2"; v2.0 (2022, the last release) waits rather than drops
when out of buffers and ignores echo_id; master 2024-01..2026-01 has bugs on
this very path (7fba5612 frames from the host into a wrong buffer, f418aa36
lost messages to the host; fixed by d97f265e, 0080001f, PR 209/223), and
sellers flash master. The async build (8 OUT transfers back to back) was
worse than the sync one, which fits a race in the adapter.

Branch `async-send` `0fa7851` (run 39425363678), done as the Linux driver
does: 32 receive transfers of whole packets with WinUSB RAW_IO (libusb
1.0.30 in MSYS2; guarded off for 1.0.29 on Linux); at most 10 frames in the
adapter un-echoed (50 ms reclaim); features logged (firmware age); first log
line names the commits. Switches: `bin/candle.cfg` (`tx_in_flight=`,
`raw_io=`, `pad_packets=`) and `candle-test -f N -r 0|1 -P`.

**Asked of the owner:** the load self-test on Windows (no drive) in several
settings, then the drive with the best one:
`candle-test -m loopback -b 500000 -s 123#0000000000000000 -c -p 1 -t 20`
with `-f 10` (default), `-f 1`, `-r 0`, `-P`; the same on Linux with sudo
for comparison; then the GUI with adpt-bike and log.txt. If losses stay on
every setting, the adapter firmware is the likely culprit: flashing current
candleLight master (DFU) would tell.

**2026-10-23:** the owner says the problem stays with `0fa7851`, but the log
brought (`log (4).txt`) is a byte copy of `log (3).txt`, so the new build's
log was not seen; `data_*.csv` there are upstream's watch values, no link
state. Redirecting candle-test output by hand left no file, so `307b9b9`
adds `bin/selftest.cmd` (double click: four loopback load runs into
`bin/selftest.txt`; checked under wine). Asked: selftest.txt on Windows,
the same command on Linux with sudo (our libusb path, no drive: tells
Windows from our protocol / the firmware), the new build's log.txt (first
line `uCAN Monitor for Windows 307b9b9`), a trace with the drive, and
whether Linux was tested with the very same adapter.

**2026-10-23, data from `307b9b9`** (`logs/selftest.txt`, `logs/log (5).txt`):
the adapter reports features 0xFB (v2.0 is 0xF3; one-shot added after v2.0,
user-id dropped 2022-11-14): mid-2022 firmware, v2.0-like paths. Loopback
load test, 20 000 frames each: default (10 in flight, RAW_IO) lost 1;
`-f 1` lost 0; `-f 0 -r 0` lost 9; `-P` lost 35; unconfirmed = lost, so a
frame vanishes inside the adapter before CAN. GUI with the drive (default
settings, but the wrong server: adpt-etk-inverter on the adpt-bike drive,
hence SDO aborts): unconfirmed 697 of 6249 (11 %), bus errors 47. Reading:
v2.0's to-host path cannot drop; the double-buffered USB OUT receive can
lose a packet arriving right behind another, and the GUI sends in bursts
(SYNC, RPDOs, SDO in one turn) while ~300 frames/s come from the bus.
Next: GUI on adpt-bike (right server) with `bin/candle.cfg` `tx_in_flight=1`;
if clean, make 1 the default and release.

**2026-10-23, `log(6).txt`** (candle.cfg `tx_in_flight=1`; server
adpt-etk-inverter, which sends the same RPDO1/RPDO2 as adpt-bike — layouts,
periods and builders are identical, only the SDO watch list differs):
unconfirmed 54 of 13 667 (0.4 %, was 11 %); every loss stalled sending 50 ms
(longest send 45-105 ms); bus errors 852 in ~2 min (0 on 2026-10-20): ask if
the motor ran, and compare with Linux `ip -details -statistics link show can0`
after a session with upstream. The drive: an invalid frame now and then, no
link loss.

`cc7da9e` on the branch: tx_in_flight=1 by default, and a frame taken by the
adapter but not echoed in 10 ms is sent again (at most twice; same echo_id;
a second echo counts as `late echoes`, i.e. on the bus twice); event thread
wakes every 5 ms. selftest.cmd now: default, `-R 0`, `-f 10`, `-f 0 -r 0`.
If the owner sees resent > 0, unconfirmed 0, late echoes 0 and no drive
warnings: merge to main (release).

**2026-10-23, `log(7).txt`** (307b9b9, tx_in_flight=1, no resend; watch
period switched from 10 ms to 1 ms): at 1 ms the queue grew (27), then
unconfirmed 292 and refused 2448 in 10 s, then sent 0 for good while still
receiving: the adapter stopped taking frames (candleLight #207's symptom).
The 50 ms flow reclaim kept adding frames while earlier ones waited in the
adapter, until its 64 buffers ran out; the Linux driver never has over 10
out. `97608d8`: a hard limit of 8 un-echoed frames in the adapter, given
up ones included for 2 s; one at a time while it keeps up, up to 4 when a
frame waited 2 ms; resend with any window, frames of an id wait while one
of it may be resent. candle-test `-n K` bursts with ids counting up;
loopback check by time, per-id order and duplicates.

Firmware route if software is not enough: prebuilt master binaries are the
artifacts of candleLight_fw's CI; flash with
`dfu-util -D <fw>.bin -a 0 -s 0x08000000:leave`; the board must be known
(candleLight, CANable-MKS 1.0, DSD TECH SH-C30A, FYSETC UCAN... all F072,
different pins), and every colleague's adapter would need it.

**2026-10-28, `logs/28_10_26/`** (97608d8). Owner: 10 ms clean for
minutes; 1 ms one invalid frame in 3 min. Long run, 32 min (26 at 1 ms):
~2900 sent / 10 s steady, dropped 0 throughout, queue 6-20 (max 56),
resent 1-4 %, `late echoes` 0, `unconfirmed` 1-2 about 30 times (lost
through both resends: the invalid frames); bus errors up to 1110 / 10 s,
then none after ~22 min. Ended with "adapter is gone" as the laptop dimmed
its screen; reopened, the adapter received nothing (needs a replug).
Load self-test: one at a time 1868 frames/s, lossless; window 4 1565/s with
70 unconfirmed; window 4 with resend under overload collapsed to 346/s
(1925 resends, 473 late echoes). `56a9bfd`: tx_in_flight=1 default, 4
resends, keep_awake (SetThreadExecutionState system+display while the
event thread runs), replug hint in the log.

**Reading them:**

- no warnings or stops, no `CAN, last 10 s` lines: fixed. Rebase `async-send`
  onto `main`, push `main` (the CI releases), delete the branch, remove this
  section;
- `unconfirmed by the adapter` above 0 (M lines in the trace): the adapter
  drops frames it took. Do as the Linux gs_usb driver: keep at most 10
  frames un-echoed in the adapter, send the next only after an echo;
- `failed` or `dropped` above 0: the USB path still loses frames; the error
  text says how;
- `bus errors` above 0 (X lines): the bus itself; a counter repeated (not
  skipped) in the drive's eyes fits double reception after an error;
- a long `longest pause`: the uCANopen thread stalls. Candidates: Windows 11
  throttles the timer resolution of minimized or hidden windows (see
  `SetProcessInformation`, `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`);
  thread priority;
- warnings without any `CAN` lines: the frames leave fine; look at the
  adapter, the bus, or upstream timing.
