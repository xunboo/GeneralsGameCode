"""Run base-faction self-play and repeatability checks with installed game data.

These synthetic replays check that the controller operates in the real engine.
They contain no CRC expectations and do not measure strength against the legacy
AI. The game installation is read as data; its executable is never replaced.
"""

import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re
import subprocess
import time

from make_smoke_replay import FACTIONS, make_fixture


DEFAULT_MAPS = ["tournament desert", "final crusade", "tournament city"]
ENGINE_SIDES = {"USA": "America", "China": "China", "GLA": "GLA"}
CASE_PATTERN = re.compile(r'Simulating Replay "(.+?)"')
METRIC_PATTERN = re.compile(r"(\w+)=([^\s]+)")


def parse_metrics(log: str) -> dict[str, list[dict]]:
    games = {}
    current = None
    for line in log.splitlines():
        case = CASE_PATTERN.search(line)
        if case:
            current = Path(case.group(1)).stem
            games[current] = []
        elif line.startswith("CompetitiveAI ") and current is not None:
            values = dict(METRIC_PATTERN.findall(line))
            games[current].append({key: value if key == "side" else int(value)
                                   for key, value in values.items()})
    return games


def check_case(samples: list[dict], sides: tuple[str, str], seconds: int) -> tuple[list, list]:
    errors = []
    players = []
    for index, faction in enumerate(sides):
        records = [sample for sample in samples if sample["player"] == index + 2]
        if not records:
            errors.append(f"{faction}: no simulation telemetry")
            continue
        first = records[0]
        if first["cash"] != 10000 or first["owned"] != 2 or first["earned"] != 0:
            errors.append(f"{faction}: unexpected starting economy or objects")
        if first["side"] != ENGINE_SIDES[faction]:
            errors.append(f"{faction}: wrong faction instantiated")
        if max(record["income"] for record in records) <= 0:
            errors.append(f"{faction}: did not collect income")
        if max(record["combat"] for record in records) <= 0:
            errors.append(f"{faction}: did not produce combat units")
        if len(records) < 3:
            errors.append(f"{faction}: stopped before two minutes of telemetry")
        if any(record["cash"] < 0 for record in records):
            errors.append(f"{faction}: invalid negative cash")
        players.append({
            "faction": faction,
            "last_simulation_second": records[-1]["frame"] // 30,
            "peak_income_per_minute": max(record["income"] for record in records),
            "peak_combat_units": max(record["combat"] for record in records),
            "peak_supplies": max(record["supplies"] for record in records),
            "peak_loaded_transports": max(record["loaded"] for record in records),
            "reached_tech": any(record["tech"] > 0 for record in records),
            "units_lost": records[-1]["unitsLost"],
            "units_killed": records[-1]["unitsKilled"],
            "money_earned": records[-1]["earned"],
        })
    if seconds >= 180 and len(samples) < 6:
        errors.append("insufficient simulation samples")
    return players, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/competitive-ai/suite"))
    parser.add_argument("--maps", nargs="+", default=DEFAULT_MAPS)
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=600,
                        help="Wall-clock timeout for the entire engine process")
    args = parser.parse_args()
    executable = args.exe.resolve()
    game_dir = args.game_dir.resolve()
    output = args.output.resolve()
    if not executable.is_file() or not (game_dir / "INIZH.big").is_file():
        parser.error("The built Zero Hour executable and installed game data must exist")
    if args.seconds < 180:
        parser.error("Use at least 180 simulation seconds for the opening checks")
    output.mkdir(parents=True, exist_ok=True)

    fixtures = []
    for map_index, map_name in enumerate(args.maps):
        for sides in itertools.product(FACTIONS, repeat=2):
            key = f"map{map_index}-{sides[0]}-{sides[1]}"
            fixture = output / f"{key}.rep"
            fixture.write_bytes(make_fixture(map_name, list(sides), args.seconds, args.seed))
            fixtures.append((key, map_name, sides, fixture))
    # Each faction leads a repeated match, with all games reset in the same process.
    repeats = [("USA", "China"), ("China", "GLA"), ("GLA", "USA")]
    for sides in repeats:
        key = f"repeat-{sides[0]}-{sides[1]}"
        fixture = output / f"{key}.rep"
        fixture.write_bytes(make_fixture(args.maps[0], list(sides), args.seconds, args.seed))
        fixtures.append((key, args.maps[0], sides, fixture))

    command = [str(executable), "-headless", "-setCwd", str(game_dir)]
    for _, _, _, fixture in fixtures:
        command.extend(["-replay", str(fixture)])
    print(f"Running {len(fixtures)} real-engine scenarios, {args.seconds} simulation seconds each",
          flush=True)
    started = time.monotonic()
    with (output / "engine.log").open("wb") as stdout, (output / "engine.err.log").open("wb") as stderr:
        try:
            result = subprocess.run(command, cwd=game_dir, stdout=stdout, stderr=stderr,
                                    timeout=args.timeout, check=False)
        except subprocess.TimeoutExpired:
            print(f"Engine timed out; diagnostics are in {output}", flush=True)
            return 1
    elapsed = time.monotonic() - started
    log = (output / "engine.log").read_text(encoding="latin1")
    metrics = parse_metrics(log)
    errors = []
    cases = []
    if result.returncode != 0 or "Simulation of all replays completed. Errors occurred: 0" not in log:
        errors.append(f"Engine did not report success (exit {result.returncode})")
    for key, map_name, sides, _ in fixtures:
        players, case_errors = check_case(metrics.get(key, []), sides, args.seconds)
        cases.append({"case": key, "map": map_name, "players": players, "errors": case_errors})
        errors.extend(f"{key}: {error}" for error in case_errors)
    deterministic = []
    for sides in repeats:
        original = metrics.get(f"map0-{sides[0]}-{sides[1]}", [])
        repeated = metrics.get(f"repeat-{sides[0]}-{sides[1]}", [])
        equal = bool(original) and original == repeated
        deterministic.append({"sides": sides, "telemetry_matches": equal})
        if not equal:
            errors.append(f"Repeat {sides}: telemetry differs with the same map and seed")
    report = {
        "purpose": "runtime self-play smoke tests; not a comparative win-rate benchmark",
        "executable": str(executable),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "game_data_directory": str(game_dir),
        "simulation_seconds_per_case": args.seconds,
        "seed": args.seed, "wall_seconds": round(elapsed, 2),
        "engine_exit_code": result.returncode,
        "cases": cases, "repeatability": deterministic, "errors": errors,
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Completed in {elapsed:.1f}s: {len(cases)} scenarios, {len(errors)} check failures",
          flush=True)
    for error in errors:
        print(error)
    print(f"Report: {output / 'report.json'}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
