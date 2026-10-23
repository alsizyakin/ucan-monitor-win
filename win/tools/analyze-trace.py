#!/usr/bin/env python3
"""Reads a CAN trace of ucan-monitor-win (bin/cantrace-*.csv, or candle-test -w)
and tells where frames went missing.

    python3 win/tools/analyze-trace.py cantrace.csv [--node 1]

For the node's RPDO1 and RPDO2 (0x200 + node, 0x300 + node), which carry a
2-bit counter in the low bits of byte 7, it checks the counter and the period
as the program sent them (S) and as the adapter reported them sent on the bus
(E). It lists the frames the adapter never confirmed (M), lost in USB (F) or
refused (D), the bus errors (X), and the moments the node raised its warning,
error or critical flag (TPDO1, 0x180 + node, byte 1 bits 1-3), each with the
RPDO trouble just before it.
"""

import argparse
import collections
import csv
import statistics

EVENTS = {
    "S": "sent by the program",
    "A": "sent again, the adapter lost it",
    "E": "confirmed by the adapter",
    "M": "never confirmed by the adapter",
    "F": "lost in a failed USB transfer",
    "D": "refused, send queue full",
    "R": "received",
    "X": "bus errors",
}


def read(path):
    rows = []
    with open(path, newline="", encoding="ascii") as f:
        for r in csv.DictReader(f):
            rows.append((float(r["time_ms"]), r["event"], r["echo_id"],
                         int(r["can_id"], 16), int(r["len"]), bytes.fromhex(r["data"])))
    return rows


def stream(rows, event, can_id):
    """(time, counter) of the 8-byte frames with can_id and event."""
    return [(t, d[7] & 0x3) for t, e, _, c, n, d in rows if e == event and c == can_id and n == 8]


def check_stream(name, frames, trouble):
    if len(frames) < 2:
        print(f"  {name}: {len(frames)} frames")
        return
    periods = [b[0] - a[0] for a, b in zip(frames, frames[1:])]
    breaks = [(b[0], a[1], b[1], b[0] - a[0]) for a, b in zip(frames, frames[1:])
              if b[1] != (a[1] + 1) & 0x3]
    print(f"  {name}: {len(frames)} frames, period {statistics.mean(periods):.1f} ms "
          f"(max {max(periods):.1f}), counter breaks {len(breaks)}")
    for t, prev, cur, dt in breaks[:15]:
        print(f"    {t / 1000:9.3f} s  counter {prev} -> {cur}, {dt:.1f} ms after the one before")
    if len(breaks) > 15:
        print(f"    ... {len(breaks) - 15} more")
    long = [(b[0], b[0] - a[0]) for a, b in zip(frames, frames[1:]) if b[0] - a[0] > 250]
    for t, dt in long[:15]:
        print(f"    {t / 1000:9.3f} s  {dt:.1f} ms without a frame")
    trouble += [(t, f"{name}: counter {prev} -> {cur}") for t, prev, cur, _ in breaks]
    trouble += [(t, f"{name}: {dt:.0f} ms gap") for t, dt in long]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--node", type=lambda x: int(x, 0), default=1, help="the device's node id (default 1)")
    args = ap.parse_args()

    rows = read(args.trace)
    if not rows:
        print("empty trace")
        return
    print(f"{args.trace}: {len(rows)} events over {rows[-1][0] / 1000:.1f} s")
    counts = collections.Counter(e for _, e, *_ in rows)
    for e, what in EVENTS.items():
        if counts[e]:
            print(f"  {e} {what}: {counts[e]}")

    # time from the program sending a frame to the adapter confirming it
    sent_at = {}
    latency = []
    for t, e, echo, c, _, _ in rows:
        if e == "S":
            sent_at[echo] = t
        elif e == "E" and echo in sent_at:
            latency.append((t - sent_at.pop(echo), t, c))
    if latency:
        ms = sorted(x[0] for x in latency)
        worst = max(latency)
        print(f"adapter confirmation after sending: median {ms[len(ms) // 2]:.2f} ms, "
              f"99 % {ms[int(len(ms) * 0.99)]:.2f} ms, max {worst[0]:.2f} ms "
              f"at {worst[1] / 1000:.3f} s (id {worst[2]:X})")

    trouble = []
    for e in "MFD":
        frames = [(t, c, d) for t, ee, _, c, _, d in rows if ee == e]
        if frames:
            print(f"{len(frames)} frame{'s' if len(frames) != 1 else ''} {EVENTS[e]}:")
            for t, c, d in frames[:15]:
                print(f"  {t / 1000:9.3f} s  id {c:X}  {d.hex().upper()}")
            trouble += [(t, f"{EVENTS[e]}: id {c:X}") for t, c, _ in frames]

    errors = [(t, c, d) for t, e, _, c, _, d in rows if e == "X"]
    if errors:
        print(f"{len(errors)} bus error frames, {errors[0][0] / 1000:.3f} s to {errors[-1][0] / 1000:.3f} s:")
        for t, c, d in errors[:15]:
            print(f"  {t / 1000:9.3f} s  class {c & 0x1FFFFFFF:X}  {d.hex().upper()}")
        trouble += [(t, f"bus error class {c & 0x1FFFFFFF:X}") for t, c, _ in errors]

    for name, can_id in (("RPDO1", 0x200 + args.node), ("RPDO2", 0x300 + args.node)):
        print(f"{name} (id {can_id:X}):")
        check_stream("as the program sent it", stream(rows, "S", can_id), trouble)
        check_stream("as the adapter sent it on the bus", stream(rows, "E", can_id), trouble)

    # the node's warning / error / critical flags in TPDO1
    tpdo1 = [(t, d) for t, e, _, c, n, d in rows if e == "R" and c == 0x180 + args.node and n >= 2]
    names = {1: "warning", 2: "error", 3: "critical"}
    trouble.sort()
    previous = 0
    raised = 0
    print(f"node {args.node} flags (TPDO1 {0x180 + args.node:X}, {len(tpdo1)} frames):")
    for t, d in tpdo1:
        flags = d[1] & 0x0E
        rising = flags & ~previous
        if rising:
            raised += 1
            what = ", ".join(names[b] for b in (1, 2, 3) if rising >> b & 1)
            before = [x for x in trouble if t - 1000 <= x[0] <= t]
            print(f"  {t / 1000:9.3f} s  {what} raised; in the second before: "
                  + ("; ".join(f"{x[1]} at {x[0] / 1000:.3f}" for x in before[-5:]) or "nothing seen"))
        previous = flags
    if not raised:
        print("  never raised")


if __name__ == "__main__":
    main()
