#!/usr/bin/env python3
"""Analyze quota-stopped depth=2 op-specific State experiments."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path

import analyze_conditional_experiment as common


PROFILES = ("BASELINE", "READ_D2", "WRITE_D2", "ALL_D2")
STATES = ("NORMAL", "READ_SLOW", "WRITE_SLOW", "MIXED_SLOW")
LIMITS = {
    "BASELINE": (4, 4),
    "READ_D2": (2, 4),
    "WRITE_D2": (4, 2),
    "ALL_D2": (2, 2),
}
GUARDS = {
    profile: (int(read_limit < 4), int(write_limit < 4))
    for profile, (read_limit, write_limit) in LIMITS.items()
}
READ_STATE_THRESHOLD_NS = 2_207_192
WRITE_STATE_THRESHOLD_NS = 9_090_039
FORMAL_QUOTA = 8
MAX_BLOCKS = 96
TARGET_STATES = {
    "READ_D2": "READ_SLOW",
    "WRITE_D2": "WRITE_SLOW",
    "ALL_D2": "MIXED_SLOW",
}
CONTROLLED_IOPS = {
    "READ_D2": ("bg_read_iops",),
    "WRITE_D2": ("bg_write_iops",),
    "ALL_D2": ("bg_read_iops", "bg_write_iops"),
}
METRICS = common.METRICS

# Reuse the validated request/phase parser with this experiment's profile map.
common.PROFILES = PROFILES
common.PROFILE_GUARDS = GUARDS
common.PROFILE_LIMITS = LIMITS


def load_schedule(path: Path) -> list[dict[str, int | str]]:
    rows: list[dict[str, int | str]] = []
    columns = (
        "block", "position", "profile", "read_guard", "write_guard",
        "read_limit", "write_limit", "trial_seed",
    )
    with path.open(newline="", encoding="ascii") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != columns:
            raise ValueError(f"unexpected schedule columns: {reader.fieldnames}")
        for raw in reader:
            profile = raw["profile"]
            if profile not in PROFILES:
                raise ValueError(f"invalid schedule profile: {profile}")
            row: dict[str, int | str] = {
                "block": int(raw["block"]),
                "position": int(raw["position"]),
                "profile": profile,
                "read_guard": int(raw["read_guard"]),
                "write_guard": int(raw["write_guard"]),
                "read_limit": int(raw["read_limit"]),
                "write_limit": int(raw["write_limit"]),
                "trial_seed": int(raw["trial_seed"]),
            }
            if (row["read_guard"], row["write_guard"]) != GUARDS[profile]:
                raise ValueError(f"guard flags do not match {profile}")
            if (row["read_limit"], row["write_limit"]) != LIMITS[profile]:
                raise ValueError(f"limits do not match {profile}")
            rows.append(row)
    validate_blocks(rows)
    return rows


def validate_blocks(rows: list[dict[str, int | str]]) -> None:
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


def validate_balanced_prefix(
    rows: list[dict[str, int | str]], pilot: bool,
) -> list[str]:
    errors: list[str] = []
    blocks = sorted({int(row["block"]) for row in rows})
    if pilot:
        if len(blocks) != 1:
            errors.append(f"pilot blocks {len(blocks)} != 1")
        return errors
    if not blocks or len(blocks) % 4:
        errors.append("formal completed block count is not a positive multiple of four")
        return errors
    expected = len(blocks) // 4
    positions = Counter(
        (str(row["profile"]), int(row["position"])) for row in rows
    )
    for profile in PROFILES:
        for position in range(1, 5):
            if positions[(profile, position)] != expected:
                errors.append(
                    f"{profile} position {position} count "
                    f"{positions[(profile, position)]} != {expected}"
                )
    carryover: Counter[tuple[str, str]] = Counter()
    for block in blocks:
        ordered = sorted(
            (row for row in rows if int(row["block"]) == block),
            key=lambda row: int(row["position"]),
        )
        carryover.update(
            (str(previous["profile"]), str(current["profile"]))
            for previous, current in zip(ordered, ordered[1:])
        )
    for previous in PROFILES:
        for current in PROFILES:
            target = 0 if previous == current else expected
            if carryover[(previous, current)] != target:
                errors.append(
                    f"carryover {previous}->{current} "
                    f"{carryover[(previous, current)]} != {target}"
                )
    return errors


def classify_state(read_p99_ns: int, write_p99_ns: int) -> str:
    read_slow = read_p99_ns >= READ_STATE_THRESHOLD_NS
    write_slow = write_p99_ns >= WRITE_STATE_THRESHOLD_NS
    if read_slow and write_slow:
        return "MIXED_SLOW"
    if read_slow:
        return "READ_SLOW"
    if write_slow:
        return "WRITE_SLOW"
    return "NORMAL"


def load_enrollment(
    path: Path,
    schedule: list[dict[str, int | str]],
) -> tuple[list[dict[str, int | str]], list[str]]:
    columns = (
        "block", "position", "profile", "state", "pre_fg_p99_ns",
        "pre_bg_read_p99_ns", "pre_bg_write_p99_ns",
    )
    rows: list[dict[str, int | str]] = []
    errors: list[str] = []
    with path.open(newline="", encoding="ascii") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != columns:
            raise ValueError(f"unexpected enrollment columns: {reader.fieldnames}")
        for raw in reader:
            row: dict[str, int | str] = {
                "block": int(raw["block"]),
                "position": int(raw["position"]),
                "profile": raw["profile"],
                "state": raw["state"],
                "pre_fg_p99_ns": int(raw["pre_fg_p99_ns"]),
                "pre_bg_read_p99_ns": int(raw["pre_bg_read_p99_ns"]),
                "pre_bg_write_p99_ns": int(raw["pre_bg_write_p99_ns"]),
            }
            rows.append(row)
    if not rows:
        return rows, ["enrollment.csv has no trials"]
    if len(rows) > len(schedule):
        errors.append("enrollment has more rows than schedule")
    for index, (row, expected) in enumerate(zip(rows, schedule), 1):
        for name in ("block", "position", "profile"):
            if row[name] != expected[name]:
                errors.append(f"enrollment row {index} {name} mismatch")
        calculated = classify_state(
            int(row["pre_bg_read_p99_ns"]),
            int(row["pre_bg_write_p99_ns"]),
        )
        if row["state"] != calculated:
            errors.append(
                f"enrollment row {index} state {row['state']} != {calculated}"
            )
    if len(rows) % 4:
        errors.append("runner stopped with a partial block")
    return rows, errors


def parse_stdout_state(
    path: Path,
) -> tuple[dict[str, int | str], list[str]]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    matches = [line for line in lines if line.startswith("pre_state=")]
    if len(matches) != 1:
        return {}, [f"{path.name}: expected one pre_state line"]
    raw = common.key_values(matches[0])
    errors: list[str] = []
    result: dict[str, int | str] = {"state": raw.get("pre_state", "")}
    for name in (
        "pre_fg_p99_ns", "pre_bg_read_p99_ns", "pre_bg_write_p99_ns",
        "read_state_threshold_ns", "write_state_threshold_ns",
    ):
        try:
            result[name] = int(raw[name])
        except (KeyError, ValueError):
            errors.append(f"{path.name}: missing valid {name}")
    if result.get("read_state_threshold_ns") != READ_STATE_THRESHOLD_NS:
        errors.append(f"{path.name}: READ State threshold mismatch")
    if result.get("write_state_threshold_ns") != WRITE_STATE_THRESHOLD_NS:
        errors.append(f"{path.name}: WRITE State threshold mismatch")
    return result, errors


def validate_config(config: dict[str, str], pilot: bool) -> list[str]:
    errors: list[str] = []
    expected = {
        "read_state_threshold_ns": str(READ_STATE_THRESHOLD_NS),
        "write_state_threshold_ns": str(WRITE_STATE_THRESHOLD_NS),
        "cell_quota": "0" if pilot else str(FORMAL_QUOTA),
    }
    for name, value in expected.items():
        if config.get(name) != value:
            errors.append(f"config {name}={config.get(name)!r} != {value!r}")
    return errors


def verify_quota_stop(
    trials: list[dict[str, int | float | str]],
    input_dir: Path,
    pilot: bool,
) -> list[str]:
    if pilot:
        return []
    if not trials:
        return ["formal run has no complete trials"]
    errors: list[str] = []
    counts = Counter((str(row["profile"]), str(row["state"])) for row in trials)
    for profile in PROFILES:
        for state in STATES:
            if counts[(profile, state)] < FORMAL_QUOTA:
                errors.append(
                    f"cell {profile}/{state} count {counts[(profile, state)]} "
                    f"< {FORMAL_QUOTA}"
                )
    blocks = max(int(row["block"]) for row in trials)
    marker = input_dir / "quota-complete-block.txt"
    if not marker.is_file():
        errors.append("missing quota-complete-block.txt")
    else:
        try:
            marker_block = int(marker.read_text(encoding="ascii").strip())
        except ValueError:
            errors.append("invalid quota-complete-block.txt")
        else:
            if marker_block != blocks:
                errors.append(f"quota marker block {marker_block} != {blocks}")
    if blocks > MAX_BLOCKS:
        errors.append(f"completed blocks {blocks} > maximum {MAX_BLOCKS}")
    if blocks >= 8:
        previous = [row for row in trials if int(row["block"]) <= blocks - 4]
        previous_counts = Counter(
            (str(row["profile"]), str(row["state"])) for row in previous
        )
        if all(
            previous_counts[(profile, state)] >= FORMAL_QUOTA
            for profile in PROFILES for state in STATES
        ):
            errors.append("runner continued past the first balanced quota boundary")
    return errors


def make_cell_summary(
    trials: list[dict[str, int | float | str]],
) -> list[dict[str, int | str]]:
    counts = Counter((str(row["profile"]), str(row["state"])) for row in trials)
    return [
        {
            "profile": profile,
            "state": state,
            "trials": counts[(profile, state)],
        }
        for profile in PROFILES for state in STATES
    ]


def profile_state_summary(
    trials: list[dict[str, int | float | str]],
) -> list[dict[str, int | float | str]]:
    rows = []
    for profile in PROFILES:
        for state in STATES:
            selected = [
                row for row in trials
                if row["profile"] == profile and row["state"] == state
            ]
            row: dict[str, int | float | str] = {
                "profile": profile,
                "state": state,
                "trials": len(selected),
            }
            for metric in METRICS:
                values = [float(trial[metric]) for trial in selected]
                row[f"{metric}_mean"] = statistics.fmean(values)
                row[f"{metric}_median"] = statistics.median(values)
            rows.append(row)
    return rows


def aggregate_cell(
    by_block: dict[int, list[dict[str, int | float | str]]],
    draw: tuple[int, ...],
    profile: str,
    state: str,
    metric: str,
) -> tuple[float, int]:
    total = 0.0
    count = 0
    for block in draw:
        for trial in by_block[block]:
            if trial["profile"] == profile and trial["state"] == state:
                total += float(trial[metric])
                count += 1
    return total, count


def conditional_effects(
    trials: list[dict[str, int | float | str]],
    draws: list[tuple[int, ...]],
) -> list[dict[str, int | float | str | None]]:
    by_block: dict[int, list[dict[str, int | float | str]]] = defaultdict(list)
    for trial in trials:
        by_block[int(trial["block"])].append(trial)
    observed_draw = tuple(sorted(by_block))
    output: list[dict[str, int | float | str | None]] = []
    for metric in METRICS:
        for profile in PROFILES[1:]:
            observed: dict[str, tuple[float, int, int] | None] = {}
            boot: dict[str, list[float]] = {state: [] for state in STATES}
            target = TARGET_STATES[profile]
            modifier_boot: list[float] = []
            for state in STATES:
                action_sum, action_n = aggregate_cell(
                    by_block, observed_draw, profile, state, metric
                )
                base_sum, base_n = aggregate_cell(
                    by_block, observed_draw, "BASELINE", state, metric
                )
                observed[state] = (
                    (action_sum / action_n - base_sum / base_n, action_n, base_n)
                    if action_n and base_n else None
                )
            for draw in draws:
                effects: dict[str, float] = {}
                for state in STATES:
                    action_sum, action_n = aggregate_cell(
                        by_block, draw, profile, state, metric
                    )
                    base_sum, base_n = aggregate_cell(
                        by_block, draw, "BASELINE", state, metric
                    )
                    if action_n and base_n:
                        effects[state] = (
                            action_sum / action_n - base_sum / base_n
                        )
                        boot[state].append(effects[state])
                if target in effects and "NORMAL" in effects:
                    modifier_boot.append(effects[target] - effects["NORMAL"])
            for state in STATES:
                selected = observed[state]
                low, high = common.percentile_ci(boot[state])
                output.append({
                    "metric": metric,
                    "profile": profile,
                    "estimand": state,
                    "state": state,
                    "action_n": selected[1] if selected is not None else 0,
                    "baseline_n": selected[2] if selected is not None else 0,
                    "mean_effect": selected[0] if selected is not None else None,
                    "ci95_low": low,
                    "ci95_high": high,
                    "valid_bootstrap_replicates": len(boot[state]),
                })
            modifier = None
            if observed[target] is not None and observed["NORMAL"] is not None:
                modifier = observed[target][0] - observed["NORMAL"][0]
            low, high = common.percentile_ci(modifier_boot)
            output.append({
                "metric": metric,
                "profile": profile,
                "estimand": "TARGET_MINUS_NORMAL",
                "state": target,
                "action_n": observed[target][1] if observed[target] else 0,
                "baseline_n": observed[target][2] if observed[target] else 0,
                "mean_effect": modifier,
                "ci95_low": low,
                "ci95_high": high,
                "valid_bootstrap_replicates": len(modifier_boot),
            })
    return output


def unconditional_effects(
    trials: list[dict[str, int | float | str]],
    draws: list[tuple[int, ...]],
) -> list[dict[str, int | float | str | None]]:
    by_block: dict[int, dict[str, dict[str, int | float | str]]] = defaultdict(dict)
    for trial in trials:
        by_block[int(trial["block"])][str(trial["profile"])] = trial
    output: list[dict[str, int | float | str | None]] = []
    for metric in METRICS:
        for profile in PROFILES[1:]:
            values = {
                block: (
                    float(rows[profile][metric])
                    - float(rows["BASELINE"][metric])
                )
                for block, rows in by_block.items()
            }
            observed = [values[block] for block in sorted(values)]
            bootstrap = [
                statistics.fmean(values[block] for block in draw)
                for draw in draws
            ]
            low, high = common.percentile_ci(bootstrap)
            output.append({
                "metric": metric,
                "contrast": f"{profile}_vs_BASELINE",
                "blocks": len(observed),
                "mean_effect": statistics.fmean(observed),
                "median_effect": statistics.median(observed),
                "ci95_low": low,
                "ci95_high": high,
                "negative_blocks": sum(value < 0 for value in observed),
                "positive_blocks": sum(value > 0 for value in observed),
                "valid_bootstrap_replicates": len(bootstrap),
            })
    return output


def assessments(
    trials: list[dict[str, int | float | str]],
    effects: list[dict[str, int | float | str | None]],
    iterations: int,
    quality_ok: bool,
) -> list[dict[str, int | float | str | bool | None]]:
    counts = Counter((str(row["profile"]), str(row["state"])) for row in trials)
    cells_ok = all(
        counts[(profile, state)] >= FORMAL_QUOTA
        for profile in PROFILES for state in STATES
    )
    lookup = {
        (str(row["profile"]), str(row["estimand"])): row
        for row in effects if row["metric"] == "fg_p99_ns"
    }
    output = []
    for profile in PROFILES[1:]:
        target = TARGET_STATES[profile]
        target_row = lookup[(profile, target)]
        modifier_row = lookup[(profile, "TARGET_MINUS_NORMAL")]
        bootstrap_ok = (
            int(target_row["valid_bootstrap_replicates"])
            >= math.ceil(iterations * common.BOOTSTRAP_MIN_VALID_RATIO)
            and int(modifier_row["valid_bootstrap_replicates"])
            >= math.ceil(iterations * common.BOOTSTRAP_MIN_VALID_RATIO)
        )
        target_benefit = (
            target_row["ci95_high"] is not None
            and float(target_row["ci95_high"]) < 0
        )
        modifier_benefit = (
            modifier_row["ci95_high"] is not None
            and float(modifier_row["ci95_high"]) < 0
        )
        ratios = []
        for metric in CONTROLLED_IOPS[profile]:
            action_values = [
                float(row[metric]) for row in trials
                if row["profile"] == profile and row["state"] == target
            ]
            base_values = [
                float(row[metric]) for row in trials
                if row["profile"] == "BASELINE" and row["state"] == target
            ]
            ratios.append(
                statistics.fmean(action_values) / statistics.fmean(base_values)
            )
        throughput_ratio = min(ratios)
        throughput_ok = throughput_ratio >= 0.5
        supported = (
            quality_ok and cells_ok and bootstrap_ok and target_benefit
            and modifier_benefit and throughput_ok
        )
        output.append({
            "profile": profile,
            "target_state": target,
            "quality_gate": quality_ok,
            "cell_count_gate": cells_ok,
            "bootstrap_validity_gate": bootstrap_ok,
            "target_benefit_gate": target_benefit,
            "modifier_benefit_gate": modifier_benefit,
            "throughput_gate": throughput_ok,
            "controlled_iops_ratio": throughput_ratio,
            "supported": supported,
            "target_effect_ns": target_row["mean_effect"],
            "target_ci95_low_ns": target_row["ci95_low"],
            "target_ci95_high_ns": target_row["ci95_high"],
            "modifier_ns": modifier_row["mean_effect"],
            "modifier_ci95_low_ns": modifier_row["ci95_low"],
            "modifier_ci95_high_ns": modifier_row["ci95_high"],
        })
    return output


def fmt_ns(value: object) -> str:
    return "N/A" if value is None else f"{float(value) / 1000.0:,.1f} us"


def build_report(
    pilot: bool,
    trials: list[dict[str, int | float | str]],
    cells: list[dict[str, int | str]],
    effects: list[dict[str, int | float | str | None]],
    checks: list[dict[str, int | float | str | bool | None]],
    errors: list[str],
) -> str:
    blocks = len({int(row["block"]) for row in trials})
    lines = [
        "# Depth=2 op-specific State 随机实验报告",
        "",
        "## 有效性",
        "",
        f"- 模式：{'pilot' if pilot else 'formal'}。",
        f"- 完成 trial：{len(trials)}；完整 block：{blocks}。",
        f"- 数据质量错误：{len(errors)}。",
        f"- READ/WRITE State 阈值：{READ_STATE_THRESHOLD_NS} / "
        f"{WRITE_STATE_THRESHOLD_NS} ns。",
        "",
        "## Cell 数量",
        "",
        "| Profile | NORMAL | READ_SLOW | WRITE_SLOW | MIXED_SLOW |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    cell_lookup = {
        (str(row["profile"]), str(row["state"])): int(row["trials"])
        for row in cells
    }
    for profile in PROFILES:
        lines.append(
            f"| {profile} | "
            + " | ".join(str(cell_lookup[(profile, state)]) for state in STATES)
            + " |"
        )
    if pilot:
        lines.extend([
            "",
            "## 判定",
            "",
            "pilot 只验证 depth=2 并发、State 分类和质量门，不形成 CATE 结论。",
        ])
    else:
        lookup = {
            (str(row["profile"]), str(row["estimand"])): row
            for row in effects if row["metric"] == "fg_p99_ns"
        }
        lines.extend([
            "",
            "## 主要匹配 CATE",
            "",
            "负值表示 Action 降低前台 P99。",
            "",
            "| Action | State/Estimand | 效应 | 95% CI |",
            "| --- | --- | ---: | ---: |",
        ])
        for profile in PROFILES[1:]:
            target = TARGET_STATES[profile]
            for estimand in (target, "TARGET_MINUS_NORMAL"):
                row = lookup[(profile, estimand)]
                lines.append(
                    f"| {profile} | {estimand} | {fmt_ns(row['mean_effect'])} | "
                    f"[{fmt_ns(row['ci95_low'])}, {fmt_ns(row['ci95_high'])}] |"
                )
        lines.extend(["", "## 预注册判定", ""])
        supported = [row for row in checks if bool(row["supported"])]
        if supported:
            lines.append("满足全部门槛、可进入短反馈 E2 的 Action：")
            lines.extend(f"- {row['profile']}" for row in supported)
        else:
            lines.append("没有 Action 满足全部预注册门槛。")
        for row in checks:
            lines.append(
                f"- {row['profile']}: target={row['target_benefit_gate']}, "
                f"modifier={row['modifier_benefit_gate']}, "
                f"throughput={row['throughput_gate']}。"
            )
    if errors:
        lines.extend(["", "## 数据质量错误", ""])
        lines.extend(f"- {error}" for error in errors[:100])
    lines.extend([
        "",
        "本实验不识别 UFS 内部原因，也不代表正式内核已有 depth=2 Action。",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule", type=Path, required=True)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--bootstrap", type=int, default=20_000)
    parser.add_argument("--seed", type=int, default=2026080112)
    parser.add_argument("--pilot", action="store_true")
    args = parser.parse_args()
    if args.bootstrap < 1000:
        raise ValueError("bootstrap iterations must be at least 1000")

    schedule = load_schedule(args.schedule)
    config, foreground_uid = common.parse_config(args.input_dir)
    errors = common.validate_run_files(args.input_dir, args.schedule, config)
    errors.extend(validate_config(config, args.pilot))
    if not args.pilot:
        if len(schedule) != MAX_BLOCKS * 4:
            errors.append(
                f"maximum schedule trials {len(schedule)} != {MAX_BLOCKS * 4}"
            )
        errors.extend(validate_balanced_prefix(schedule, False))
    enrollment, enrollment_errors = load_enrollment(
        args.input_dir / "enrollment.csv", schedule
    )
    errors.extend(enrollment_errors)
    completed_schedule = schedule[:len(enrollment)]
    errors.extend(validate_balanced_prefix(completed_schedule, args.pilot))

    trials = []
    trial_errors: dict[str, list[str]] = {}
    for expected, enrolled in zip(completed_schedule, enrollment):
        trial, current_errors = common.parse_trial(
            args.input_dir, expected, foreground_uid
        )
        stdout_state, stdout_errors = parse_stdout_state(
            args.input_dir / f"{trial['trial']}-stdout.txt"
        )
        current_errors.extend(stdout_errors)
        if "pre_pressure" in trial:
            calculated = classify_state(
                int(trial["pre_bg_read_p99_ns"]),
                int(trial["pre_bg_write_p99_ns"]),
            )
            trial["state"] = calculated
            for name in (
                "pre_fg_p99_ns", "pre_bg_read_p99_ns", "pre_bg_write_p99_ns",
            ):
                if int(trial[name]) != int(enrolled[name]):
                    current_errors.append(
                        f"{trial['trial']}: enrollment {name} mismatch"
                    )
                if name in stdout_state and int(trial[name]) != int(stdout_state[name]):
                    current_errors.append(
                        f"{trial['trial']}: stdout {name} mismatch"
                    )
            if enrolled["state"] != calculated or stdout_state.get("state") != calculated:
                current_errors.append(f"{trial['trial']}: State classification mismatch")
        current_errors = sorted(set(current_errors))
        trial["quality_error_count"] = len(current_errors)
        trials.append(trial)
        if current_errors:
            trial_errors[str(trial["trial"])] = current_errors
            errors.extend(current_errors)
    complete_trials = [
        row for row in trials
        if "state" in row and all(metric in row for metric in METRICS)
    ]
    if len(complete_trials) != len(enrollment):
        errors.append(
            f"complete trials {len(complete_trials)} != enrolled {len(enrollment)}"
        )
    errors.extend(verify_quota_stop(complete_trials, args.input_dir, args.pilot))
    errors = sorted(set(errors))

    cells = make_cell_summary(complete_trials) if complete_trials else []
    profile_states = (
        profile_state_summary(complete_trials)
        if complete_trials and all(int(row["trials"]) for row in cells) else []
    )
    profile_overall = (
        common.profile_summary(complete_trials)
        if complete_trials and all(
            any(row["profile"] == profile for row in complete_trials)
            for profile in PROFILES
        ) else []
    )
    unconditional: list[dict[str, int | float | str | None]] = []
    conditional: list[dict[str, int | float | str | None]] = []
    checks: list[dict[str, int | float | str | bool | None]] = []
    formal_ready = (
        not args.pilot and len(complete_trials) == len(enrollment)
        and complete_trials and all(int(row["trials"]) >= FORMAL_QUOTA for row in cells)
    )
    if formal_ready:
        block_ids = sorted({int(row["block"]) for row in complete_trials})
        draws = common.make_bootstrap_draws(block_ids, args.bootstrap, args.seed)
        unconditional = unconditional_effects(complete_trials, draws)
        conditional = conditional_effects(complete_trials, draws)
        checks = assessments(
            complete_trials, conditional, args.bootstrap, not errors
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    common.write_csv(args.output_dir / "trial-summary.csv", complete_trials)
    common.write_csv(args.output_dir / "cell-summary.csv", cells)
    common.write_csv(args.output_dir / "profile-state-summary.csv", profile_states)
    common.write_csv(args.output_dir / "profile-summary.csv", profile_overall)
    common.write_csv(args.output_dir / "unconditional-effects.csv", unconditional)
    common.write_csv(args.output_dir / "conditional-effects.csv", conditional)
    summary = {
        "mode": "pilot" if args.pilot else "formal",
        "scheduled_max_trials": len(schedule),
        "completed_trials": len(complete_trials),
        "completed_blocks": len({int(row["block"]) for row in complete_trials}),
        "cell_quota": 0 if args.pilot else FORMAL_QUOTA,
        "read_state_threshold_ns": READ_STATE_THRESHOLD_NS,
        "write_state_threshold_ns": WRITE_STATE_THRESHOLD_NS,
        "bootstrap_iterations": 0 if args.pilot else args.bootstrap,
        "bootstrap_seed": None if args.pilot else args.seed,
        "foreground_uid": foreground_uid,
        "data_quality_errors": errors,
        "trial_errors": trial_errors,
        "cells": cells,
        "profile_states": profile_states,
        "profile_summary": profile_overall,
        "assessments": checks,
        "supported_candidates": [
            row["profile"] for row in checks if bool(row["supported"])
        ],
        "unconditional_effects": unconditional,
        "conditional_effects": conditional,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=True, allow_nan=False) + "\n",
        encoding="ascii",
    )
    (args.output_dir / "report.md").write_text(
        build_report(
            args.pilot, complete_trials, cells, conditional, checks, errors
        ),
        encoding="utf-8",
    )
    print(
        f"mode={'pilot' if args.pilot else 'formal'} "
        f"completed={len(complete_trials)} "
        f"blocks={len({int(row['block']) for row in complete_trials})} "
        f"errors={len(errors)} output={args.output_dir}"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
