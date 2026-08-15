#!/usr/bin/env python3
"""Analyze randomized block READ/WRITE guard interventions."""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import re
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Callable


PROFILES = ("BASELINE", "READ_GUARD", "WRITE_GUARD", "ALL_GUARD")
ROLES = ("foreground_read", "background_read", "background_write")
EXPECTED_WORKERS = {
    "foreground_read": {0},
    "background_read": {0, 1, 2, 3},
    "background_write": {0, 1, 2, 3},
}
EXPECTED_IOPRIO = {
    "foreground_read": 2,
    "background_read": 3,
    "background_write": 3,
}
MEASURE_SECONDS = 2.0
SLOW_THRESHOLD_NS = 1_500_000
GATE_DEADLINE_NS = 2_000_000_000
MIN_FOREGROUND_SAMPLES = 1000


def nearest_rank(values: list[int], quantile: float) -> int:
    if not values:
        raise ValueError("empty sample")
    ordered = sorted(values)
    rank = max(1, math.ceil(quantile * len(ordered)))
    return ordered[rank - 1]


def max_overlap(intervals: list[tuple[int, int]]) -> int:
    events: list[tuple[int, int]] = []
    for start, end in intervals:
        events.append((start, 1))
        events.append((end, -1))
    current = 0
    maximum = 0
    for _, delta in sorted(events, key=lambda event: (event[0], event[1])):
        current += delta
        maximum = max(maximum, current)
        if current < 0:
            raise ValueError("invalid interval ordering")
    return maximum


def load_schedule(path: Path) -> list[dict[str, int | str]]:
    rows: list[dict[str, int | str]] = []
    with path.open(newline="", encoding="ascii") as handle:
        reader = csv.DictReader(handle)
        expected = {
            "block", "position", "profile", "read_guard", "write_guard",
            "trial_seed",
        }
        if set(reader.fieldnames or ()) != expected:
            raise ValueError(f"unexpected schedule columns: {reader.fieldnames}")
        for row in reader:
            rows.append({
                "block": int(row["block"]),
                "position": int(row["position"]),
                "profile": row["profile"],
                "read_guard": int(row["read_guard"]),
                "write_guard": int(row["write_guard"]),
                "trial_seed": int(row["trial_seed"]),
            })
    blocks = sorted({int(row["block"]) for row in rows})
    if not blocks or blocks != list(range(1, len(blocks) + 1)):
        raise ValueError("schedule block numbering is incomplete")
    for block in blocks:
        trials = [row for row in rows if int(row["block"]) == block]
        if len(trials) != 4:
            raise ValueError(f"block {block} does not have four trials")
        if {str(row["profile"]) for row in trials} != set(PROFILES):
            raise ValueError(f"block {block} does not contain all profiles")
        if {int(row["position"]) for row in trials} != {1, 2, 3, 4}:
            raise ValueError(f"block {block} positions are incomplete")
        if len({int(row["trial_seed"]) for row in trials}) != 1:
            raise ValueError(f"block {block} does not share one I/O seed")
    return rows


def parse_stats(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8", errors="replace")
    patterns = {
        "current_depth": r"current_depth:(\d+)",
        "meta_inuse": r"metadata:capacity=\d+,inuse=(\d+)",
        "fg_dispatched": r"dispatched:special=\d+,fg=(\d+),bg=\d+",
        "bg_dispatched": r"dispatched:special=\d+,fg=\d+,bg=(\d+)",
        "depth_anomalies": r"depth_anomalies:(\d+)",
        "allocation_failures": r"allocation_failures:(\d+)",
        "inference": r"inference:(\d+)",
        "timeout": r"timeout:(\d+)",
        "invalid": r"invalid:(\d+)",
    }
    result: dict[str, int] = {}
    for name, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise ValueError(f"missing {name} in {path}")
        result[name] = int(match.group(1))
    return result


def parse_battery_temp(path: Path) -> int | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"battery_temp_tenths_c=(\d+)", text)
    return int(match.group(1)) if match else None


def parse_foreground_uid(input_dir: Path) -> int:
    path = input_dir / "config-before.txt"
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^fg_uid=(\d+)$", text, re.MULTILINE)
    if not match or int(match.group(1)) <= 0:
        raise ValueError(f"missing positive framework fg_uid in {path}")
    return int(match.group(1))


def parse_trial(
    input_dir: Path,
    expected: dict[str, int | str],
    foreground_uid: int,
) -> tuple[dict[str, int | float | str], list[str]]:
    block = int(expected["block"])
    position = int(expected["position"])
    profile = str(expected["profile"])
    trial = f"b{block:02d}-p{position}-{profile}"
    csv_path = input_dir / f"{trial}.csv"
    errors: list[str] = []
    if not csv_path.is_file():
        return ({
            "block": block,
            "position": position,
            "profile": profile,
            "trial": trial,
        }, [f"missing {csv_path}"])

    latencies: dict[str, list[int]] = defaultdict(list)
    gate_waits: dict[str, list[int]] = defaultdict(list)
    intervals: dict[str, list[tuple[int, int]]] = defaultdict(list)
    workers: dict[str, set[int]] = defaultdict(set)
    row_count = 0
    with csv_path.open(newline="", encoding="ascii") as handle:
        reader = csv.DictReader(handle)
        required = {
            "profile", "read_guard", "write_guard", "role", "op", "worker",
            "sequence", "offset_bytes", "size_bytes", "gate_wait_ns",
            "start_ns", "end_ns", "latency_ns", "ret", "errno",
            "ioprio_class", "submit_uid",
        }
        if set(reader.fieldnames or ()) != required:
            return ({
                "block": block,
                "position": position,
                "profile": profile,
                "trial": trial,
            }, [f"unexpected CSV columns in {csv_path}"])
        for row in reader:
            row_count += 1
            role = row["role"]
            if role not in ROLES:
                errors.append(f"{trial}: invalid role {role}")
                continue
            if row["profile"] != profile:
                errors.append(f"{trial}: profile mismatch")
            if int(row["read_guard"]) != int(expected["read_guard"]):
                errors.append(f"{trial}: read_guard mismatch")
            if int(row["write_guard"]) != int(expected["write_guard"]):
                errors.append(f"{trial}: write_guard mismatch")
            expected_op = "write" if role == "background_write" else "read"
            if row["op"] != expected_op:
                errors.append(f"{trial}: role/op mismatch")
            worker = int(row["worker"])
            workers[role].add(worker)
            if int(row["ioprio_class"]) != EXPECTED_IOPRIO[role]:
                errors.append(f"{trial}: ioprio mismatch for {role}")
            expected_uid = foreground_uid if role == "foreground_read" else 0
            if int(row["submit_uid"]) != expected_uid:
                errors.append(f"{trial}: UID mismatch for {role}")
            if int(row["ret"]) != 4096 or int(row["errno"]) != 0:
                errors.append(f"{trial}: failed or short I/O")
            start = int(row["start_ns"])
            end = int(row["end_ns"])
            latency = int(row["latency_ns"])
            gate_wait = int(row["gate_wait_ns"])
            if end < start or latency != end - start:
                errors.append(f"{trial}: invalid latency timestamps")
            if gate_wait < 0 or gate_wait >= GATE_DEADLINE_NS:
                errors.append(f"{trial}: guard wait reached deadline")
            if not int(expected["read_guard"]) and role == "background_read" and gate_wait:
                errors.append(f"{trial}: unguarded READ has gate wait")
            if not int(expected["write_guard"]) and role == "background_write" and gate_wait:
                errors.append(f"{trial}: unguarded WRITE has gate wait")
            latencies[role].append(latency)
            gate_waits[role].append(gate_wait)
            intervals[role].append((start, end))

    if not row_count:
        errors.append(f"{trial}: no samples")
    for role, expected_workers in EXPECTED_WORKERS.items():
        if workers[role] != expected_workers:
            errors.append(
                f"{trial}: {role} workers {sorted(workers[role])} "
                f"!= {sorted(expected_workers)}"
            )
    if len(latencies["foreground_read"]) < MIN_FOREGROUND_SAMPLES:
        errors.append(
            f"{trial}: foreground samples {len(latencies['foreground_read'])} "
            f"< {MIN_FOREGROUND_SAMPLES}"
        )

    read_concurrency = max_overlap(intervals["background_read"])
    write_concurrency = max_overlap(intervals["background_write"])
    if int(expected["read_guard"]):
        if read_concurrency > 1:
            errors.append(f"{trial}: READ_GUARD concurrency {read_concurrency} > 1")
    elif read_concurrency < 2:
        errors.append(f"{trial}: unguarded READ concurrency {read_concurrency} < 2")
    if int(expected["write_guard"]):
        if write_concurrency > 1:
            errors.append(f"{trial}: WRITE_GUARD concurrency {write_concurrency} > 1")
    elif write_concurrency < 2:
        errors.append(f"{trial}: unguarded WRITE concurrency {write_concurrency} < 2")

    stats_before_path = input_dir / f"{trial}-stats-before.txt"
    stats_after_path = input_dir / f"{trial}-stats-after.txt"
    if not stats_before_path.is_file() or not stats_after_path.is_file():
        errors.append(f"{trial}: missing module stats")
        stats_delta = {name: 0 for name in (
            "fg_dispatched", "bg_dispatched", "depth_anomalies",
            "allocation_failures", "inference", "timeout", "invalid",
        )}
        stats_after = {"current_depth": 0, "meta_inuse": 0}
    else:
        stats_before = parse_stats(stats_before_path)
        stats_after = parse_stats(stats_after_path)
        stats_delta = {
            name: stats_after[name] - stats_before[name]
            for name in (
                "fg_dispatched", "bg_dispatched", "depth_anomalies",
                "allocation_failures", "inference", "timeout", "invalid",
            )
        }
        if stats_delta["fg_dispatched"] <= 0:
            errors.append(f"{trial}: no foreground dispatches classified")
        if stats_delta["bg_dispatched"] <= 0:
            errors.append(f"{trial}: no background dispatches classified")
        for name in (
            "depth_anomalies", "allocation_failures", "inference",
            "timeout", "invalid",
        ):
            if stats_delta[name]:
                errors.append(f"{trial}: module {name} delta={stats_delta[name]}")
        if stats_after["current_depth"] or stats_after["meta_inuse"]:
            errors.append(
                f"{trial}: unsettled module state depth={stats_after['current_depth']} "
                f"meta={stats_after['meta_inuse']}"
            )

    before_thermal = input_dir / f"{trial}-before-thermal.txt"
    after_thermal = input_dir / f"{trial}-after-thermal.txt"
    before_temp = parse_battery_temp(before_thermal) if before_thermal.is_file() else None
    after_temp = parse_battery_temp(after_thermal) if after_thermal.is_file() else None
    if before_temp is not None and before_temp >= 420:
        errors.append(f"{trial}: started above thermal gate ({before_temp})")
    if after_temp is not None and after_temp >= 450:
        errors.append(f"{trial}: ended at hard thermal stop ({after_temp})")

    result: dict[str, int | float | str] = {
        "block": block,
        "position": position,
        "profile": profile,
        "read_guard": int(expected["read_guard"]),
        "write_guard": int(expected["write_guard"]),
        "trial_seed": int(expected["trial_seed"]),
        "trial": trial,
        "row_count": row_count,
        "battery_temp_before_tenths_c": before_temp if before_temp is not None else -1,
        "battery_temp_after_tenths_c": after_temp if after_temp is not None else -1,
        "fg_dispatch_delta": stats_delta["fg_dispatched"],
        "bg_dispatch_delta": stats_delta["bg_dispatched"],
        "read_max_concurrency": read_concurrency,
        "write_max_concurrency": write_concurrency,
    }
    for role in ROLES:
        values = latencies[role]
        prefix = {
            "foreground_read": "fg",
            "background_read": "bg_read",
            "background_write": "bg_write",
        }[role]
        if not values:
            errors.append(f"{trial}: no {role} samples")
            continue
        result[f"{prefix}_count"] = len(values)
        result[f"{prefix}_iops"] = len(values) / MEASURE_SECONDS
        result[f"{prefix}_mean_ns"] = statistics.fmean(values)
        result[f"{prefix}_p50_ns"] = nearest_rank(values, 0.50)
        result[f"{prefix}_p95_ns"] = nearest_rank(values, 0.95)
        result[f"{prefix}_p99_ns"] = nearest_rank(values, 0.99)
        result[f"{prefix}_p999_ns"] = nearest_rank(values, 0.999)
        result[f"{prefix}_max_ns"] = max(values)
        result[f"{prefix}_slow_ratio"] = (
            sum(value >= SLOW_THRESHOLD_NS for value in values) / len(values)
        )
        result[f"{prefix}_gate_p99_ns"] = nearest_rank(gate_waits[role], 0.99)
        result[f"{prefix}_gate_max_ns"] = max(gate_waits[role])
    result["bg_total_iops"] = (
        float(result.get("bg_read_iops", 0.0)) +
        float(result.get("bg_write_iops", 0.0))
    )
    return result, sorted(set(errors))


def bootstrap_mean(
    values: list[float],
    rng: random.Random,
    iterations: int,
) -> tuple[float, float]:
    samples = []
    for _ in range(iterations):
        samples.append(statistics.fmean(rng.choice(values) for _ in values))
    samples.sort()
    low = samples[math.floor(0.025 * (iterations - 1))]
    high = samples[math.ceil(0.975 * (iterations - 1))]
    return low, high


def summarize_effects(
    trials: list[dict[str, int | float | str]],
    metric: str,
    iterations: int,
    rng: random.Random,
) -> list[dict[str, int | float | str]]:
    by_block: dict[int, dict[str, float]] = defaultdict(dict)
    for trial in trials:
        by_block[int(trial["block"])][str(trial["profile"])] = float(trial[metric])

    contrast_functions: dict[str, Callable[[dict[str, float]], float]] = {
        "READ_GUARD_vs_BASELINE": lambda p: p["READ_GUARD"] - p["BASELINE"],
        "WRITE_GUARD_vs_BASELINE": lambda p: p["WRITE_GUARD"] - p["BASELINE"],
        "ALL_GUARD_vs_BASELINE": lambda p: p["ALL_GUARD"] - p["BASELINE"],
        "read_guard_main_effect": lambda p: (
            p["READ_GUARD"] - p["BASELINE"] +
            p["ALL_GUARD"] - p["WRITE_GUARD"]
        ) / 2.0,
        "write_guard_main_effect": lambda p: (
            p["WRITE_GUARD"] - p["BASELINE"] +
            p["ALL_GUARD"] - p["READ_GUARD"]
        ) / 2.0,
        "guard_interaction": lambda p: (
            p["ALL_GUARD"] - p["READ_GUARD"] -
            p["WRITE_GUARD"] + p["BASELINE"]
        ),
    }
    summaries = []
    for contrast, function in contrast_functions.items():
        values = [function(by_block[block]) for block in sorted(by_block)]
        low, high = bootstrap_mean(values, rng, iterations)
        summaries.append({
            "metric": metric,
            "contrast": contrast,
            "blocks": len(values),
            "mean_effect": statistics.fmean(values),
            "median_effect": statistics.median(values),
            "ci95_low": low,
            "ci95_high": high,
            "negative_blocks": sum(value < 0 for value in values),
            "positive_blocks": sum(value > 0 for value in values),
            "zero_blocks": sum(value == 0 for value in values),
        })
    return summaries


def profile_summary(
    trials: list[dict[str, int | float | str]],
    metrics: tuple[str, ...],
) -> list[dict[str, int | float | str]]:
    output = []
    for profile in PROFILES:
        profile_trials = [trial for trial in trials if trial["profile"] == profile]
        row: dict[str, int | float | str] = {
            "profile": profile,
            "trials": len(profile_trials),
        }
        for metric in metrics:
            values = [float(trial[metric]) for trial in profile_trials]
            row[f"{metric}_mean"] = statistics.fmean(values)
            row[f"{metric}_median"] = statistics.median(values)
        output.append(row)
    return output


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        raise ValueError(f"cannot write empty CSV: {path}")
    with path.open("w", newline="", encoding="ascii") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def fmt_ns(value: float) -> str:
    return f"{value / 1000.0:,.1f} us"


def build_report(
    schedule: Path,
    trials: list[dict[str, int | float | str]],
    profiles: list[dict[str, int | float | str]],
    effects: list[dict[str, int | float | str]],
    errors: list[str],
) -> str:
    lines = [
        "# READ/WRITE Guard 随机区组实验报告",
        "",
        "## 有效性",
        "",
        f"- 预注册随机表：{schedule.name}。",
        f"- 完成 trial：{len(trials)}；完整 block：{len(trials) // 4}。",
        f"- 数据质量错误：{len(errors)}。",
        "- 独立实验单元是 block，不是单条 I/O。",
        "",
        "## Profile 汇总",
        "",
        "| Profile | 前台 READ P99 | 前台慢比例 | 前台 IOPS | 后台 READ IOPS | 后台 WRITE IOPS |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in profiles:
        lines.append(
            f"| {row['profile']} | {fmt_ns(float(row['fg_p99_ns_mean']))} | "
            f"{100.0 * float(row['fg_slow_ratio_mean']):.2f}% | "
            f"{float(row['fg_iops_mean']):,.1f} | "
            f"{float(row['bg_read_iops_mean']):,.1f} | "
            f"{float(row['bg_write_iops_mean']):,.1f} |"
        )
    lines.extend([
        "",
        "## 前台 P99 配对效应",
        "",
        "负值表示 Guard 降低前台 READ P99；区间为按 block 重采样的 95% bootstrap CI。",
        "",
        "| Contrast | 均值效应 | 95% CI | 负向 block |",
        "| --- | ---: | ---: | ---: |",
    ])
    for row in effects:
        if row["metric"] != "fg_p99_ns":
            continue
        lines.append(
            f"| {row['contrast']} | {fmt_ns(float(row['mean_effect']))} | "
            f"[{fmt_ns(float(row['ci95_low']))}, "
            f"{fmt_ns(float(row['ci95_high']))}] | "
            f"{row['negative_blocks']}/{row['blocks']} |"
        )
    lines.extend(["", "## 判定", ""])
    if errors:
        lines.append("实验数据质量门未通过，不能作 Action 因果结论。")
        lines.extend(f"- {error}" for error in errors[:50])
    else:
        pairwise = {
            str(row["contrast"]): row
            for row in effects
            if row["metric"] == "fg_p99_ns" and "_vs_BASELINE" in str(row["contrast"])
        }
        supported = [
            name for name, row in pairwise.items()
            if float(row["ci95_high"]) < 0 and int(row["negative_blocks"]) >= 8
        ]
        if supported:
            lines.append(
                "以下 profile 同时满足预注册门槛：前台 P99 配对效应的 "
                "95% CI 上界小于 0，且至少 8/12 block 方向一致："
            )
            lines.extend(f"- {name}" for name in supported)
        else:
            lines.append(
                "没有 profile 同时满足预注册的一致性和区间门槛，当前数据不支持"
                "宣称某个 Guard 稳定改善前台 P99。"
            )
        lines.append(
            "后台吞吐和等待是部署代价；本实验只验证主机提交边界的 per-op "
            "Guard，不代表正式内核已有对应 Action。"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule", type=Path, required=True)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--bootstrap", type=int, default=20000)
    parser.add_argument("--seed", type=int, default=2026080102)
    args = parser.parse_args()

    if args.bootstrap < 1000:
        raise ValueError("bootstrap iterations must be at least 1000")
    schedule = load_schedule(args.schedule)
    foreground_uid = parse_foreground_uid(args.input_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    trials = []
    errors = []
    for expected in schedule:
        trial, trial_errors = parse_trial(args.input_dir, expected, foreground_uid)
        trials.append(trial)
        errors.extend(trial_errors)
    complete_trials = [
        trial for trial in trials
        if "fg_p99_ns" in trial and "bg_read_p99_ns" in trial
        and "bg_write_p99_ns" in trial
    ]
    if len(complete_trials) != len(schedule):
        errors.append(
            f"complete trials {len(complete_trials)} != scheduled {len(schedule)}"
        )

    metrics = (
        "fg_p99_ns", "fg_p50_ns", "fg_slow_ratio", "fg_iops",
        "bg_read_iops", "bg_write_iops", "bg_total_iops",
        "bg_read_p99_ns", "bg_write_p99_ns",
    )
    profiles = profile_summary(complete_trials, metrics) if complete_trials else []
    effects: list[dict[str, int | float | str]] = []
    if len(complete_trials) == len(schedule):
        rng = random.Random(args.seed)
        for metric in metrics:
            effects.extend(
                summarize_effects(complete_trials, metric, args.bootstrap, rng)
            )

    if complete_trials:
        write_csv(args.output_dir / "trial-summary.csv", complete_trials)
    if profiles:
        write_csv(args.output_dir / "profile-summary.csv", profiles)
    if effects:
        write_csv(args.output_dir / "effect-summary.csv", effects)
    summary = {
        "scheduled_trials": len(schedule),
        "complete_trials": len(complete_trials),
        "blocks": len(schedule) // 4,
        "bootstrap_iterations": args.bootstrap,
        "bootstrap_seed": args.seed,
        "foreground_uid": foreground_uid,
        "data_quality_errors": sorted(set(errors)),
        "profiles": profiles,
        "effects": effects,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=True) + "\n",
        encoding="ascii",
    )
    report = build_report(
        args.schedule, complete_trials, profiles, effects, sorted(set(errors))
    )
    (args.output_dir / "report.md").write_text(report, encoding="utf-8")
    print(
        f"scheduled={len(schedule)} complete={len(complete_trials)} "
        f"errors={len(set(errors))} output={args.output_dir}"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
