"""Create an AI-only Zero Hour simulation fixture using the normal replay loader.

This is a synthetic scenario, not a retail replay or a win-rate benchmark. It
contains no human orders, CRC expectations, rule overrides or scripted income.
Run with a Release build: generalszh.exe -headless -replay <absolute fixture>.
"""

import argparse
from pathlib import Path
import struct


FACTIONS = {"USA": 2, "China": 3, "GLA": 4}


def make_fixture(map_name: str, sides: list[str], seconds: int, seed: int,
                 swap_starts: bool = False) -> bytes:
    if seconds <= 0 or not 0 <= seed <= 0x7FFFFFFF:
        raise ValueError("Duration must be positive and seed must fit a signed integer")
    if len(sides) != 2 or any(side not in FACTIONS for side in sides):
        raise ValueError("Exactly two supported base factions are required")
    if any(character in map_name for character in ";=\0"):
        raise ValueError("Invalid map name")
    frames = seconds * 30
    slots = ":".join(
        f"CF,{index},{FACTIONS[side]},{1 - index if swap_starts else index},-1"
        for index, side in enumerate(sides)
    ) + ":" + "X:" * (8 - len(sides))
    options = (
        f"US=0;M=00maps/{map_name};MC=0;MS=0;SD={seed};C=999999;"
        f"SR=0;SC=10000;O=N;S={slots};"
    ).encode("ascii")

    def wide(value: str) -> bytes:
        return value.encode("utf-16-le") + b"\0\0"

    header = (
        b"GENREP" + struct.pack("<III", 0, 0, frames) + b"\0" * 10
        + wide("Competitive AI synthetic smoke test") + b"\0" * 16
        + wide("Version 1.04") + wide("Synthetic test")
        + struct.pack("<III", 0x10004, 0, 0) + options + b"\0-1\0"
        + struct.pack("<iiii", 1, 2, 0, 0)
    )
    # An empty network marker at the deadline is discarded by the recorder.
    # EOF then ends simulation; the AI controls every frame before the deadline.
    return header + struct.pack("<iiiB", frames, 1000, -1, 0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--map", default="tournament desert")
    parser.add_argument("--sides", nargs=2, choices=FACTIONS, default=["USA", "China"])
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--swap-starts", action="store_true")
    args = parser.parse_args()
    fixture = make_fixture(args.map, args.sides, args.seconds, args.seed, args.swap_starts)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(fixture)
    print(f"Created {args.output.resolve()} ({args.seconds} simulation seconds)")


if __name__ == "__main__":
    main()
