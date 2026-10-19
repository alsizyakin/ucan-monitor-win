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

**The owner will bring next:** log.txt and `cantrace-*.csv` of a Windows run
with adpt-bike until the warning shows. Read the CAN lines and run the
analyzer: unconfirmed frames point at the adapter (then limit un-echoed
frames as Linux does, below); bus errors or a clean confirmed stream
with the warning still raised point at the bus or the drive.

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
