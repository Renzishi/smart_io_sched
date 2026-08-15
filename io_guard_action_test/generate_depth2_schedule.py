#!/usr/bin/env python3
"""Generate a balanced depth=2 op-specific intervention schedule."""

from __future__ import annotations

import argparse
import csv
import random
from collections import Counter
from pathlib import Path


PROFILES = ("BASELINE", "READ_D2", "WRITE_D2", "ALL_D2")
LIMITS = {
    "BASELINE": (4, 4),
    "READ_D2": (2, 4),
    "WRITE_D2": (4, 2),
    "ALL_D2": (2, 2),
}
WILLIAMS_ROWS = (
    (0, 1, 3, 2),
    (1, 2, 0, 3),
    (2, 3, 1, 0),
    (3, 0, 2, 1),
)


def build_schedule(blocks: int, seed: int) -> list[dict[str, int | str]]:
    if blocks <= 0 or blocks % 4:
        raise ValueError("blocks must be a positive multiple of four")
    rng = random.Random(seed)
    schedule: list[dict[str, int | str]] = []
    block_no = 0
    for _ in range(blocks // 4):
        labels = list(PROFILES)
        rows = list(WILLIAMS_ROWS)
        rng.shuffle(labels)
        rng.shuffle(rows)
        for row in rows:
            block_no += 1
            block_seed = rng.randrange(1, 2**63)
            for position, treatment_index in enumerate(row, 1):
                profile = labels[treatment_index]
                read_limit, write_limit = LIMITS[profile]
                schedule.append({
                    "block": block_no,
                    "position": position,
                    "profile": profile,
                    "read_guard": int(read_limit < 4),
                    "write_guard": int(write_limit < 4),
                    "read_limit": read_limit,
                    "write_limit": write_limit,
                    "trial_seed": block_seed,
                })
    validate_schedule(schedule, blocks)
    return schedule


def validate_schedule(
    schedule: list[dict[str, int | str]], blocks: int,
) -> None:
    by_block: dict[int, list[dict[str, int | str]]] = {}
    for trial in schedule:
        by_block.setdefault(int(trial["block"]), []).append(trial)
    if set(by_block) != set(range(1, blocks + 1)):
        raise ValueError("block numbering is incomplete")
    for block, trials in by_block.items():
        if {str(trial["profile"]) for trial in trials} != set(PROFILES):
            raise ValueError(f"block {block} does not contain all profiles")
        if {int(trial["position"]) for trial in trials} != {1, 2, 3, 4}:
            raise ValueError(f"block {block} positions are incomplete")
        if len({int(trial["trial_seed"]) for trial in trials}) != 1:
            raise ValueError(f"block {block} does not share one I/O seed")

    expected = blocks // 4
    position_counts = Counter(
        (str(trial["profile"]), int(trial["position"]))
        for trial in schedule
    )
    for profile in PROFILES:
        for position in range(1, 5):
            if position_counts[(profile, position)] != expected:
                raise ValueError("profile positions are not balanced")
    carryover: Counter[tuple[str, str]] = Counter()
    for trials in by_block.values():
        ordered = sorted(trials, key=lambda trial: int(trial["position"]))
        carryover.update(
            (str(previous["profile"]), str(current["profile"]))
            for previous, current in zip(ordered, ordered[1:])
        )
    for previous in PROFILES:
        for current in PROFILES:
            target = 0 if previous == current else expected
            if carryover[(previous, current)] != target:
                raise ValueError(
                    f"unbalanced carryover {previous}->{current}: "
                    f"{carryover[(previous, current)]} != {target}"
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--blocks", type=int, default=96)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    schedule = build_schedule(args.blocks, args.seed)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="ascii") as handle:
        writer = csv.DictWriter(handle, fieldnames=(
            "block", "position", "profile", "read_guard", "write_guard",
            "read_limit", "write_limit", "trial_seed",
        ), lineterminator="\n")
        writer.writeheader()
        writer.writerows(schedule)
    print(
        f"schedule={args.output} blocks={args.blocks} trials={len(schedule)} "
        f"seed={args.seed} position_balance={args.blocks // 4} "
        f"carryover_balance={args.blocks // 4}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
