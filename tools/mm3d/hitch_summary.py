#!/usr/bin/env python3
"""Summarizes the Hitch lines of a Dekui log (see docs/hitch-trace.md).

usage: hitch_summary.py azahar_log.txt [--top 15]

Prints how many slow frames each phase dominated, the guest functions and HLE
calls that took the most time over all slow frames, and the places (scene,
entrance) where slow frames happened.
"""
import argparse
import collections
import re

TIME = re.compile(r"^\[\s*([\d.]+)\]")
HITCH = re.compile(r"Hitch: ([\d.]+) ms frame after (\d+) smooth frames(.*)")
PHASE = re.compile(r"^ ?([a-z ]+?) ([\d.]+) ms \((\d+)(?:, max ([\d.]+))?\)$")
PLACE = re.compile(r"^ ?at (.*) for (\d+) s$")
CALL = re.compile(r"^ ?(.+) ([\d.]+) ms \((\d+)\)$")
FUNCTION = re.compile(r"fn ([0-9a-f]+) (\d+)% ~([\d.]+) ms")
GUEST = re.compile(r"aot ([\d.]+) ms = code ([\d.]+) \+ svc ([\d.]+)")

# Parts that overlap others: never "dominant".
NESTED = {"cpu", "aot svc", "present slot", "present fence", "present finish", "gpu",
          "pacing", "present submit", "present gpu", "deliver", "deliver blit",
          "swap acquire", "swap present", "gpu wait", "worker wait"}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args()

    hitches = 0
    dominant = collections.Counter()
    phase_ms = collections.Counter()
    present_parts = collections.Counter()
    functions = collections.Counter()
    calls = collections.Counter()
    call_counts = collections.Counter()
    places = collections.Counter()
    guest_code = guest_svc = 0.0
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if m := HITCH.search(line):
                hitches += 1
                phases = {}
                for part in m.group(3).split(" | ")[1:]:
                    if p := PHASE.match(part):
                        phases[p.group(1)] = float(p.group(2))
                    elif p := PLACE.match(part):
                        places[p.group(1)] += 1
                for name, ms in phases.items():
                    phase_ms[name] += ms
                    if name.startswith(("present ", "pacing", "deliver", "swap ", "gpu")):
                        present_parts[name] += ms
                top = max(((ms, n) for n, ms in phases.items() if n not in NESTED), default=None)
                if top:
                    dominant[top[1]] += 1
            elif "Hitch calls:" in line:
                for part in line.split("Hitch calls:", 1)[1].split(" | "):
                    if c := CALL.match(part.strip()):
                        calls[c.group(1)] += float(c.group(2))
                        call_counts[c.group(1)] += int(c.group(3))
            elif "Hitch guest:" in line:
                if g := GUEST.search(line):
                    guest_code += float(g.group(2))
                    guest_svc += float(g.group(3))
                for fn in FUNCTION.finditer(line):
                    functions[fn.group(1)] += float(fn.group(3))

    print(f"{hitches} slow frames")
    print("dominant phase:", ", ".join(f"{n} {c}" for n, c in dominant.most_common()))
    print("total ms per phase:", ", ".join(f"{n} {ms:.0f}" for n, ms in phase_ms.most_common()))
    if guest_code or guest_svc:
        print(f"AOT in slow frames: guest code {guest_code:.0f} ms, SVCs {guest_svc:.0f} ms")
    if functions:
        print("guest functions (estimated ms over slow frames):")
        for fn, ms in functions.most_common(args.top):
            print(f"  {fn}  {ms:8.1f}")
    if calls:
        print("HLE calls (ms, calls over slow frames):")
        for name, ms in calls.most_common(args.top):
            print(f"  {name:50s} {ms:8.1f} {call_counts[name]:7d}")
    if places:
        print("places:")
        for place, n in places.most_common(args.top):
            print(f"  {place:40s} {n}")


if __name__ == "__main__":
    main()
