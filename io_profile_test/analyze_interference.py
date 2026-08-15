#!/usr/bin/env python3
"""Paired block analysis for foreground reads under read/write background I/O."""
import argparse
import csv
import json
import random
import statistics
from pathlib import Path


def percentile(values, q):
    values = sorted(values)
    return values[int((len(values) - 1) * q)]


def stats(values):
    return {
        "count": len(values),
        "mean_ns": statistics.fmean(values),
        "p50_ns": percentile(values, 0.50),
        "p95_ns": percentile(values, 0.95),
        "p99_ns": percentile(values, 0.99),
        "max_ns": max(values),
        "slow_ge_4ms": sum(value >= 4_000_000 for value in values),
    }


def load_latencies(path):
    values = []
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            if int(row["ret"]) >= 0:
                values.append(int(row["latency_ns"]))
    if not values:
        raise ValueError(f"no valid samples in {path}")
    return values


def block_and_condition(stem):
    prefix = stem.removesuffix("-foreground-read")
    if prefix == "bgread":
        return "b01", "bg_read"
    if prefix == "bgwrite":
        return "b01", "bg_write"
    if len(prefix) == 4 and prefix.startswith("b0"):
        return prefix[:3], "bg_write" if prefix.endswith("w") else "bg_read"
    raise ValueError(f"unknown trial name {stem}")


def bootstrap_mean_ci(values, seed, iterations=10000):
    rng = random.Random(seed)
    means = []
    for _ in range(iterations):
        sample = [rng.choice(values) for _ in values]
        means.append(statistics.fmean(sample))
    means.sort()
    return [means[int(iterations * 0.025)], means[int(iterations * 0.975)]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    probe_dir = Path(args.probe_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    trials = []
    aggregate = {"bg_read": [], "bg_write": []}
    background_aggregate = {"bg_read": [], "bg_write": []}
    for path in sorted(probe_dir.glob("*-foreground-read.csv")):
        block, condition = block_and_condition(path.stem)
        values = load_latencies(path)
        prefix = path.stem.removesuffix("-foreground-read")
        background_op = "read" if condition == "bg_read" else "write"
        background_path = probe_dir / f"{prefix}-background-{background_op}.csv"
        background_values = load_latencies(background_path)
        aggregate[condition].extend(values)
        background_aggregate[condition].extend(background_values)
        row = {"block": block, "condition": condition, "file": path.name}
        row.update(stats(values))
        background_result = stats(background_values)
        row["background_file"] = background_path.name
        row["background_mean_ns"] = background_result["mean_ns"]
        row["background_p99_ns"] = background_result["p99_ns"]
        row["background_slow_ge_4ms"] = background_result["slow_ge_4ms"]
        trials.append(row)

    by_block = {}
    for row in trials:
        by_block.setdefault(row["block"], {})[row["condition"]] = row
    paired = []
    for block, conditions in sorted(by_block.items()):
        if set(conditions) != {"bg_read", "bg_write"}:
            raise ValueError(f"incomplete paired block {block}: {conditions}")
        delta = conditions["bg_write"]["p99_ns"] - conditions["bg_read"]["p99_ns"]
        paired.append({"block": block,
                       "bg_read_p99_ns": conditions["bg_read"]["p99_ns"],
                       "bg_write_p99_ns": conditions["bg_write"]["p99_ns"],
                       "delta_write_minus_read_p99_ns": delta,
                       "background_read_p99_ns":
                           conditions["bg_read"]["background_p99_ns"],
                       "background_write_p99_ns":
                           conditions["bg_write"]["background_p99_ns"],
                       "background_delta_write_minus_read_p99_ns":
                           conditions["bg_write"]["background_p99_ns"] -
                           conditions["bg_read"]["background_p99_ns"]})

    deltas = [row["delta_write_minus_read_p99_ns"] for row in paired]
    background_deltas = [
        row["background_delta_write_minus_read_p99_ns"] for row in paired
    ]
    result = {
        "trial_count": len(trials),
        "paired_block_count": len(paired),
        "aggregate": {key: stats(values) for key, values in aggregate.items()},
        "background_service": {
            key: stats(values) for key, values in background_aggregate.items()
        },
        "paired_p99_delta": {
            "values_ns": deltas,
            "mean_ns": statistics.fmean(deltas),
            "median_ns": statistics.median(deltas),
            "write_higher_blocks": sum(value > 0 for value in deltas),
            "read_higher_blocks": sum(value < 0 for value in deltas),
            "bootstrap_mean_95ci_ns": bootstrap_mean_ci(deltas, 20260731),
        },
        "paired_background_p99_delta": {
            "values_ns": background_deltas,
            "mean_ns": statistics.fmean(background_deltas),
            "median_ns": statistics.median(background_deltas),
            "write_higher_blocks": sum(value > 0 for value in background_deltas),
            "read_higher_blocks": sum(value < 0 for value in background_deltas),
            "bootstrap_mean_95ci_ns":
                bootstrap_mean_ci(background_deltas, 20260731),
        },
    }
    with (output_dir / "interference-summary.json").open("w", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
    for name, rows in (("interference-trials.csv", trials),
                       ("interference-paired.csv", paired)):
        with (output_dir / name).open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
