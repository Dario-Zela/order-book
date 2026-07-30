#!/usr/bin/env python3
"""Deterministic synthetic ITCH 5.0 file generator (seeded).

CI helper: gives perf/fuzz jobs a realistic-mix input without the 5.6GB
sample-day download. NOT a substitute for the pinned real day — synthetic
mixes flatter the caches (see the band-sizing finding in the README).

    python3 tools/gen_synth.py out.itch --msgs=3600000 --seed=20260730
"""
import argparse
import random
import struct

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--msgs", type=int, default=3_600_000)
    ap.add_argument("--symbols", type=int, default=50)
    ap.add_argument("--seed", type=int, default=20260730)
    args = ap.parse_args()

    random.seed(args.seed)
    out = bytearray()

    def frame(body: bytes) -> None:
        out.extend(struct.pack(">H", len(body)))
        out.extend(body)

    def hdr(t: bytes, locate: int, ts: int) -> bytes:
        return t + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")

    mids = [random.randint(500, 3_000_000) for _ in range(args.symbols)]
    ts = 25_200_000_000_000  # 7am
    frame(hdr(b"S", 0, ts) + b"O")
    for i in range(args.symbols):
        name = f"SYM{i:04d} ".encode()
        frame(hdr(b"R", i + 1, ts) + name + b"QN" + struct.pack(">I", 100)
              + b"NCZ PNN1N" + struct.pack(">I", 0) + b"N")

    live: list[tuple[int, int]] = []
    next_ref = 1
    frame(hdr(b"S", 0, ts) + b"Q")
    for n in range(args.msgs):
        ts += random.randint(1_000, 20_000)
        loc = random.randint(1, args.symbols)
        mid = mids[loc - 1]
        r = random.random()
        if r < 0.42 or not live:
            px = max(1, mid + random.randint(-150, 150))
            if random.random() < 0.001:
                px = mid * 3 + 500_000  # rare fat finger
            side = b"B" if random.random() < 0.5 else b"S"
            qty = random.randint(1, 10) * 100
            frame(hdr(b"A", loc, ts) + struct.pack(">Q", next_ref) + side
                  + struct.pack(">I", qty) + f"SYM{loc - 1:04d} ".encode()
                  + struct.pack(">I", px))
            live.append((next_ref, loc))
            next_ref += 1
        else:
            k = random.randrange(len(live))
            ref, oloc = live[k]
            op = random.random()
            if op < 0.30:
                frame(hdr(b"E", oloc, ts)
                      + struct.pack(">QIQ", ref, random.randint(1, 3) * 100, n))
            elif op < 0.42:
                frame(hdr(b"X", oloc, ts) + struct.pack(">QI", ref, 100))
            elif op < 0.72:
                frame(hdr(b"D", oloc, ts) + struct.pack(">Q", ref))
                live[k] = live[-1]
                live.pop()
            elif op < 0.92:
                px = max(1, mids[oloc - 1] + random.randint(-150, 150))
                frame(hdr(b"U", oloc, ts)
                      + struct.pack(">QQII", ref, next_ref,
                                    random.randint(1, 10) * 100, px))
                live[k] = (next_ref, oloc)
                next_ref += 1
            elif op < 0.97:
                frame(hdr(b"P", oloc, ts) + struct.pack(">Q", 0) + b"B"
                      + struct.pack(">I", 200) + f"SYM{oloc - 1:04d} ".encode()
                      + struct.pack(">I", mids[oloc - 1]) + struct.pack(">Q", n))
            else:
                frame(hdr(b"Q", oloc, ts) + struct.pack(">Q", 5000)
                      + f"SYM{oloc - 1:04d} ".encode()
                      + struct.pack(">I", mids[oloc - 1]) + struct.pack(">Q", n)
                      + b"O")
    frame(hdr(b"S", 0, ts) + b"M")
    with open(args.out, "wb") as f:
        f.write(out)
    print(f"{len(out) / 1e6:.1f} MB, ~{args.msgs} msgs, {len(live)} live at end")

if __name__ == "__main__":
    main()
