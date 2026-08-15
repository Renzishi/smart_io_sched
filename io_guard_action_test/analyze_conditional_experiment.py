#!/usr/bin/env python3
"""Analyze treatment-prestate-conditioned READ/WRITE guard experiments."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import random
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path
from typing import Callable


PROFILES = ("BASELINE", "READ_GUARD", "WRITE_GUARD", "ALL_GUARD")
PROFILE_GUARDS = {
    "BASELINE": (0, 0),
    "READ_GUARD": (1, 0),
    "WRITE_GUARD": (0, 1),
    "ALL_GUARD": (1, 1),
}
PROFILE_LIMITS = {
    "BASELINE": (4, 4),
    "READ_GUARD": (1, 4),
    "WRITE_GUARD": (4, 1),
    "ALL_GUARD": (1, 1),
}
ROLES = ("foreground_read", "background_read", "background_write")
ROLE_PREFIX = {
    "foreground_read": "fg",
    "background_read": "bg_read",
    "background_write": "bg_write",
}
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
CSV_COLUMNS = (
    "profile", "phase", "read_guard", "write_guard", "role", "op",
    "worker", "sequence", "offset_bytes", "size_bytes", "gate_wait_ns",
    "start_ns", "end_ns", "latency_ns", "ret", "errno",
    "ioprio_class", "submit_uid",
)
FILE_BYTES = 128 * 1024 * 1024
PRE_SECONDS = 1.0
TREATMENT_SECONDS = 2.0
SLOW_THRESHOLD_NS = 1_500_000
GATE_DEADLINE_NS = 2_000_000_000
MIN_PRE_FOREGROUND_SAMPLES = 500
MIN_TREATMENT_FOREGROUND_SAMPLES = 1000
BOOTSTRAP_MIN_VALID_RATIO = 0.95
METRICS = (
    "fg_p99_ns", "fg_p50_ns", "fg_slow_ratio", "fg_iops",
    "bg_read_p99_ns", "bg_read_iops", "bg_write_p99_ns",
    "bg_write_iops", "bg_total_iops",
)


def nearest_rank(values: list[int], quantile: float) -> int:
    if not values:
        raise ValueError("empty sample")
    ordered = sorted(values)
    rank = max(1, math.ceil(quantile * len(ordered)))
    return ordered[rank - 1]


def max_overlap(intervals: list[tuple[int, int]]) -> int:
    events: list[tuple[int, int]] = []
    for start, end in intervals:
        if end <= start:
            raise ValueError("non-positive interval")
        events.append((start, 1))
        events.append((end, -1))
    current = 0
    maximum = 0
    for _, delta in sorted(events, key=lambda event: (event[0], event[1])):
        current += delta
        if current < 0:
            raise ValueError("invalid interval ordering")
        maximum = max(maximum, current)
    if current:
        raise ValueError("unclosed interval")
    return maximum


def percentile_ci(values: list[float]) -> tuple[float | None, float | None]:
    if not values:
        return None, None
    ordered = sorted(values)
    last = len(ordered) - 1
    return ordered[math.floor(0.025 * last)], ordered[math.ceil(0.975 * last)]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_schedule(path: Path) -> list[dict[str, int | str]]:
    rows: list[dict[str, int | str]] = []
    with path.open(newline="", encoding="ascii") as handle:
        reader = csv.DictReader(handle)
        expected_columns = (
            "block", "position", "profile", "read_guard", "write_guard",
            "trial_seed",
        )
        if tuple(reader.fieldnames or ()) != expected_columns:
            raise ValueError(f"unexpected schedule columns: {reader.fieldnames}")
        for raw in reader:
            profile = raw["profile"]
            if profile not in PROFILES:
                raise ValueError(f"invalid schedule profile: {profile}")
            row = {
                "block": int(raw["block"]),
                "position": int(raw["position"]),
                "profile": profile,
                "read_guard": int(raw["read_guard"]),
                "write_guard": int(raw["write_guard"]),
                "trial_seed": int(raw["trial_seed"]),
            }
            if (row["read_guard"], row["write_guard"]) != PROFILE_GUARDS[profile]:
                raise ValueError(f"guard flags do not match {profile}")
            rows.append(row)

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


def validate_design(
    schedule: list[dict[str, int | str]], pilot: bool,
) -> list[str]:
    errors: list[str] = []
    expected_blocks = 1 if pilot else 24
    blocks = sorted({int(row["block"]) for row in schedule})
    if len(blocks) != expected_blocks:
        errors.append(
            f"schedule blocks {len(blocks)} != expected {expected_blocks}"
        )
    if pilot:
        return errors

    position_counts = Counter(
        (str(row["profile"]), int(row["position"])) for row in schedule
    )
    for profile in PROFILES:
        for position in range(1, 5):
            if position_counts[(profile, position)] != 6:
                errors.append(
                    f"schedule {profile} position {position} count "
                    f"{position_counts[(profile, position)]} != 6"
                )
    predecessor_counts: Counter[tuple[str, str]] = Counter()
    for block in blocks:
        ordered = sorted(
            (row for row in schedule if int(row["block"]) == block),
            key=lambda row: int(row["position"]),
        )
        predecessor_counts.update(
            (str(ordered[index - 1]["profile"]), str(ordered[index]["profile"]))
            for index in range(1, 4)
        )
    for previous in PROFILES:
        for current in PROFILES:
            if previous == current:
                continue
            if predecessor_counts[(previous, current)] != 6:
                errors.append(
                    f"schedule predecessor {previous}->{current} count "
                    f"{predecessor_counts[(previous, current)]} != 6"
                )
    return errors


def parse_stats(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8", errors="replace")
    patterns = {
        "current_depth": r"current_depth:(\d+)",
        "reserved_depth": r"reserved_depth:(\d+)",
        "issued_depth": r"issued_depth:(\d+)",
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


def parse_battery_temp(path: Path) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"battery_temp_tenths_c=(\d+)", text)
    if not match:
        raise ValueError(f"missing battery temperature in {path}")
    return int(match.group(1))


def key_values(text: str) -> dict[str, str]:
    return dict(re.findall(r"(?:^|\s)([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", text))


def parse_config(input_dir: Path) -> tuple[dict[str, str], int]:
    path = input_dir / "config-before.txt"
    text = path.read_text(encoding="utf-8", errors="replace")
    config: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line and not line.startswith("io_throttle"):
            name, value = line.split("=", 1)
            config[name] = value.strip()
    foreground_uid = int(config.get("fg_uid", "0"))
    if foreground_uid <= 0:
        raise ValueError(f"missing positive framework fg_uid in {path}")
    return config, foreground_uid


def validate_run_files(
    input_dir: Path,
    schedule_path: Path,
    config: dict[str, str],
) -> list[str]:
    errors: list[str] = []
    expected = {
        "throttle_enable": "1",
        "action_source": "fixed",
        "fixed_action": "NO BASELINE",
        "dev_lat": "10000000",
        "background_deadline_ms": "2000",
        "warmup_ms": "500",
        "pre_ms": "1000",
        "settle_ms": "200",
        "treatment_ms": "2000",
    }
    for name, value in expected.items():
        if config.get(name) != value:
            errors.append(
                f"config {name}={config.get(name)!r} != expected {value!r}"
            )
    if "[smart-deadline]" not in config.get("scheduler", ""):
        errors.append("config does not show selected smart-deadline scheduler")
    schedule_hash = sha256_file(schedule_path)
    if config.get("schedule_sha256") != schedule_hash:
        errors.append(
            f"schedule hash mismatch: config={config.get('schedule_sha256')} "
            f"actual={schedule_hash}"
        )
    for name in ("binary_sha256", "runner_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", config.get(name, "")):
            errors.append(f"missing valid {name} in config-before.txt")

    status_path = input_dir / "status.txt"
    if not status_path.is_file() or status_path.read_text(
        encoding="ascii", errors="replace"
    ).strip() != "complete":
        errors.append("run status.txt is missing or not complete")

    final_stats_path = input_dir / "stats-after.txt"
    if not final_stats_path.is_file():
        errors.append("missing run-level stats-after.txt")
        return errors
    try:
        final_stats = parse_stats(final_stats_path)
    except ValueError as error:
        errors.append(str(error))
        return errors
    counter_names = {
        "inference": "counter_baseline_inference",
        "timeout": "counter_baseline_timeout",
        "invalid": "counter_baseline_invalid",
        "depth_anomalies": "counter_baseline_depth_anomalies",
        "allocation_failures": "counter_baseline_allocation_failures",
    }
    for stat_name, config_name in counter_names.items():
        try:
            baseline = int(config[config_name])
        except (KeyError, ValueError):
            errors.append(f"missing valid {config_name} in config-before.txt")
            continue
        if final_stats[stat_name] != baseline:
            errors.append(
                f"run-level {stat_name} changed: {baseline}->{final_stats[stat_name]}"
            )
    for name in ("current_depth", "reserved_depth", "issued_depth", "meta_inuse"):
        if final_stats[name]:
            errors.append(f"run-level final {name}={final_stats[name]}")
    return errors


def parse_stdout(
    path: Path,
    expected: dict[str, int | str],
    foreground_uid: int,
) -> tuple[dict[str, int], dict[tuple[str, int], dict[str, str]], list[str]]:
    errors: list[str] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = [line for line in text.splitlines() if line]
    if not lines:
        return {}, {}, [f"empty stdout: {path}"]
    summary = key_values(lines[0])
    integer_names = (
        "read_guard", "write_guard", "fg_workers", "bg_read_workers",
        "bg_write_workers", "state_mode", "warmup_ms", "pre_ms",
        "settle_ms", "measure_ms", "foreground_uid", "seed",
        "load_start_ns", "pre_start_ns", "action_apply_ns",
        "measure_start_ns", "measure_end_ns", "ret",
    )
    values: dict[str, int] = {}
    for name in integer_names:
        try:
            values[name] = int(summary[name])
        except (KeyError, ValueError):
            errors.append(f"missing valid {name} in {path.name}")
    if summary.get("profile") != str(expected["profile"]):
        errors.append(f"{path.name}: stdout profile mismatch")
    expected_values = {
        "read_guard": int(expected["read_guard"]),
        "write_guard": int(expected["write_guard"]),
        "fg_workers": 1,
        "bg_read_workers": 4,
        "bg_write_workers": 4,
        "state_mode": 1,
        "warmup_ms": 500,
        "pre_ms": 1000,
        "settle_ms": 200,
        "measure_ms": 2000,
        "foreground_uid": foreground_uid,
        "seed": int(expected["trial_seed"]),
        "ret": 0,
    }
    for name, expected_value in expected_values.items():
        if values.get(name) != expected_value:
            errors.append(
                f"{path.name}: {name}={values.get(name)} != {expected_value}"
            )
    timing_checks = (
        ("load_start_ns", "pre_start_ns", 500_000_000),
        ("pre_start_ns", "action_apply_ns", 1_000_000_000),
        ("action_apply_ns", "measure_start_ns", 200_000_000),
        ("measure_start_ns", "measure_end_ns", 2_000_000_000),
    )
    for start_name, end_name, expected_delta in timing_checks:
        if start_name in values and end_name in values:
            actual_delta = values[end_name] - values[start_name]
            if actual_delta != expected_delta:
                errors.append(
                    f"{path.name}: {end_name}-{start_name}={actual_delta} "
                    f"!= {expected_delta}"
                )

    workers: dict[tuple[str, int], dict[str, str]] = {}
    for line in lines[1:]:
        if line.startswith("pre_state="):
            continue
        row = key_values(line)
        try:
            key = (row["role"], int(row["worker"]))
        except (KeyError, ValueError):
            errors.append(f"{path.name}: invalid worker summary line")
            continue
        if key in workers:
            errors.append(f"{path.name}: duplicate worker summary {key}")
        workers[key] = row
    expected_keys = {
        (role, worker)
        for role, expected_workers in EXPECTED_WORKERS.items()
        for worker in expected_workers
    }
    if set(workers) != expected_keys:
        errors.append(
            f"{path.name}: worker summaries {sorted(workers)} != "
            f"{sorted(expected_keys)}"
        )
    return values, workers, errors


def parse_meta(
    path: Path,
    expected: dict[str, int | str],
) -> list[str]:
    if not path.is_file():
        return [f"missing {path}"]
    values = key_values(path.read_text(encoding="utf-8", errors="replace"))
    errors: list[str] = []
    expected_values = {
        "block": str(expected["block"]),
        "position": str(expected["position"]),
        "profile": str(expected["profile"]),
        "read_guard": str(expected["read_guard"]),
        "write_guard": str(expected["write_guard"]),
        "seed": str(expected["trial_seed"]),
        "exit_code": "0",
    }
    for name, expected_value in expected_values.items():
        if values.get(name) != expected_value:
            errors.append(
                f"{path.name}: {name}={values.get(name)!r} != {expected_value!r}"
            )
    return errors


def add_distribution(
    result: dict[str, int | float | str],
    prefix: str,
    values: list[int],
    gate_waits: list[int],
    seconds: float,
) -> None:
    result[f"{prefix}_count"] = len(values)
    result[f"{prefix}_iops"] = len(values) / seconds
    result[f"{prefix}_mean_ns"] = statistics.fmean(values)
    result[f"{prefix}_p50_ns"] = nearest_rank(values, 0.50)
    result[f"{prefix}_p95_ns"] = nearest_rank(values, 0.95)
    result[f"{prefix}_p99_ns"] = nearest_rank(values, 0.99)
    result[f"{prefix}_p999_ns"] = nearest_rank(values, 0.999)
    result[f"{prefix}_max_ns"] = max(values)
    result[f"{prefix}_slow_ratio"] = (
        sum(value >= SLOW_THRESHOLD_NS for value in values) / len(values)
    )
    result[f"{prefix}_gate_p99_ns"] = nearest_rank(gate_waits, 0.99)
    result[f"{prefix}_gate_max_ns"] = max(gate_waits)


def parse_trial(
    input_dir: Path,
    expected: dict[str, int | str],
    foreground_uid: int,
) -> tuple[dict[str, int | float | str], list[str]]:
    block = int(expected["block"])
    position = int(expected["position"])
    profile = str(expected["profile"])
    trial = f"b{block:02d}-p{position}-{profile}"
    result: dict[str, int | float | str] = {
        "block": block,
        "position": position,
        "profile": profile,
        "read_guard": int(expected["read_guard"]),
        "write_guard": int(expected["write_guard"]),
        "trial_seed": int(expected["trial_seed"]),
        "trial": trial,
    }
    errors: set[str] = set()
    csv_path = input_dir / f"{trial}.csv"
    stdout_path = input_dir / f"{trial}-stdout.txt"
    if not csv_path.is_file():
        return result, [f"missing {csv_path}"]
    if not stdout_path.is_file():
        return result, [f"missing {stdout_path}"]

    boundaries, stdout_workers, stdout_errors = parse_stdout(
        stdout_path, expected, foreground_uid
    )
    errors.update(stdout_errors)
    errors.update(parse_meta(input_dir / f"{trial}-meta.txt", expected))
    stderr_path = input_dir / f"{trial}-stderr.txt"
    if not stderr_path.is_file():
        errors.add(f"missing {stderr_path}")
    elif stderr_path.read_text(encoding="utf-8", errors="replace").strip():
        errors.add(f"{trial}: non-empty stderr")

    latencies: dict[str, dict[str, list[int]]] = {
        phase: defaultdict(list) for phase in ("pre", "treatment")
    }
    gate_waits: dict[str, dict[str, list[int]]] = {
        phase: defaultdict(list) for phase in ("pre", "treatment")
    }
    intervals: dict[str, dict[str, list[tuple[int, int]]]] = {
        phase: defaultdict(list) for phase in ("pre", "treatment")
    }
    workers: dict[str, dict[str, set[int]]] = {
        phase: defaultdict(set) for phase in ("pre", "treatment")
    }
    worker_counts: Counter[tuple[str, str, int]] = Counter()
    sequences: dict[tuple[str, int], list[int]] = defaultdict(list)
    row_count = 0
    with csv_path.open(newline="", encoding="ascii") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != CSV_COLUMNS:
            return result, [f"unexpected CSV columns in {csv_path}"]
        for row in reader:
            row_count += 1
            phase = row["phase"]
            role = row["role"]
            if phase not in ("pre", "treatment"):
                errors.add(f"{trial}: invalid phase {phase}")
                continue
            if role not in ROLES:
                errors.add(f"{trial}: invalid role {role}")
                continue
            try:
                worker = int(row["worker"])
                sequence = int(row["sequence"])
                offset = int(row["offset_bytes"])
                size = int(row["size_bytes"])
                gate_wait = int(row["gate_wait_ns"])
                start = int(row["start_ns"])
                end = int(row["end_ns"])
                latency = int(row["latency_ns"])
                io_ret = int(row["ret"])
                saved_errno = int(row["errno"])
                ioprio = int(row["ioprio_class"])
                submit_uid = int(row["submit_uid"])
                read_guard = int(row["read_guard"])
                write_guard = int(row["write_guard"])
            except ValueError:
                errors.add(f"{trial}: non-integer CSV field")
                continue

            if row["profile"] != profile:
                errors.add(f"{trial}: profile mismatch")
            if (read_guard, write_guard) != PROFILE_GUARDS[profile]:
                errors.add(f"{trial}: guard flag mismatch")
            expected_op = "write" if role == "background_write" else "read"
            if row["op"] != expected_op:
                errors.add(f"{trial}: role/op mismatch")
            if worker not in EXPECTED_WORKERS[role]:
                errors.add(f"{trial}: invalid worker {role}/{worker}")
            workers[phase][role].add(worker)
            worker_counts[(phase, role, worker)] += 1
            sequences[(role, worker)].append(sequence)
            if ioprio != EXPECTED_IOPRIO[role]:
                errors.add(f"{trial}: ioprio mismatch for {role}")
            expected_uid = foreground_uid if role == "foreground_read" else 0
            if submit_uid != expected_uid:
                errors.add(f"{trial}: UID mismatch for {role}")
            if io_ret != 4096 or saved_errno != 0:
                errors.add(f"{trial}: failed or short I/O")
            if size != 4096 or offset < 0 or offset % 4096 or offset >= FILE_BYTES:
                errors.add(f"{trial}: invalid size or offset")
            if end <= start or latency != end - start:
                errors.add(f"{trial}: invalid latency timestamps")
                continue
            if gate_wait < 0 or gate_wait >= GATE_DEADLINE_NS:
                errors.add(f"{trial}: guard wait reached deadline")
            if phase == "pre" and gate_wait:
                errors.add(f"{trial}: pre-state I/O passed through a Guard")
            if role == "foreground_read" and gate_wait:
                errors.add(f"{trial}: foreground I/O has gate wait")
            if (
                phase == "treatment"
                and role == "background_read"
                and not int(expected["read_guard"])
                and gate_wait
            ):
                errors.add(f"{trial}: unguarded treatment READ has gate wait")
            if (
                phase == "treatment"
                and role == "background_write"
                and not int(expected["write_guard"])
                and gate_wait
            ):
                errors.add(f"{trial}: unguarded treatment WRITE has gate wait")

            if phase == "pre" and {
                "pre_start_ns", "action_apply_ns"
            }.issubset(boundaries):
                if start < boundaries["pre_start_ns"] or end > boundaries["action_apply_ns"]:
                    errors.add(f"{trial}: pre sample crosses a phase boundary")
            if phase == "treatment" and {
                "measure_start_ns", "measure_end_ns"
            }.issubset(boundaries):
                if not (
                    boundaries["measure_start_ns"] <= start
                    < boundaries["measure_end_ns"]
                ):
                    errors.add(f"{trial}: treatment sample starts outside its window")

            latencies[phase][role].append(latency)
            gate_waits[phase][role].append(gate_wait)
            intervals[phase][role].append((start, end))

    result["row_count"] = row_count
    if not row_count:
        errors.add(f"{trial}: no samples")
    for phase in ("pre", "treatment"):
        for role, expected_workers in EXPECTED_WORKERS.items():
            if workers[phase][role] != expected_workers:
                errors.add(
                    f"{trial}: {phase} {role} workers "
                    f"{sorted(workers[phase][role])} != {sorted(expected_workers)}"
                )
            if not latencies[phase][role]:
                errors.add(f"{trial}: no {phase} {role} samples")
    if len(latencies["pre"]["foreground_read"]) < MIN_PRE_FOREGROUND_SAMPLES:
        errors.add(
            f"{trial}: pre foreground samples "
            f"{len(latencies['pre']['foreground_read'])} "
            f"< {MIN_PRE_FOREGROUND_SAMPLES}"
        )
    if (
        len(latencies["treatment"]["foreground_read"])
        < MIN_TREATMENT_FOREGROUND_SAMPLES
    ):
        errors.add(
            f"{trial}: treatment foreground samples "
            f"{len(latencies['treatment']['foreground_read'])} "
            f"< {MIN_TREATMENT_FOREGROUND_SAMPLES}"
        )

    for key, values in sequences.items():
        if len(values) != len(set(values)) or values != sorted(values):
            errors.add(f"{trial}: non-monotonic sequence for {key[0]}/{key[1]}")

    concurrency: dict[tuple[str, str], int] = {}
    for phase in ("pre", "treatment"):
        for role in ("background_read", "background_write"):
            try:
                concurrency[(phase, role)] = max_overlap(intervals[phase][role])
            except ValueError as error:
                concurrency[(phase, role)] = 0
                errors.add(f"{trial}: {phase} {role} concurrency: {error}")
    if concurrency[("pre", "background_read")] < 2:
        errors.add(f"{trial}: pre READ concurrency < 2")
    if concurrency[("pre", "background_write")] < 2:
        errors.add(f"{trial}: pre WRITE concurrency < 2")
    treatment_rules = (
        ("background_read", PROFILE_LIMITS[profile][0], "READ"),
        ("background_write", PROFILE_LIMITS[profile][1], "WRITE"),
    )
    for role, limit, label in treatment_rules:
        actual = concurrency[("treatment", role)]
        if limit < 4 and actual != limit:
            errors.add(
                f"{trial}: treatment {label} concurrency {actual} != {limit}"
            )
        if limit == 4 and actual < 2:
            errors.add(f"{trial}: unguarded treatment {label} concurrency {actual} < 2")

    expected_stdout_workers = {
        (role, worker)
        for role, expected_workers in EXPECTED_WORKERS.items()
        for worker in expected_workers
    }
    for role, worker in expected_stdout_workers & set(stdout_workers):
        row = stdout_workers[(role, worker)]
        try:
            measured = int(row["measured"])
            pre_count = int(row["pre"])
            treatment_count = int(row["treatment"])
            total = int(row["total"])
            max_gate_wait = int(row["max_gate_wait_ns"])
            status = int(row["status"])
            uid = int(row["uid"])
        except (KeyError, ValueError):
            errors.add(f"{trial}: invalid stdout worker summary {role}/{worker}")
            continue
        csv_pre = worker_counts[("pre", role, worker)]
        csv_treatment = worker_counts[("treatment", role, worker)]
        if (pre_count, treatment_count, measured) != (
            csv_pre, csv_treatment, csv_pre + csv_treatment
        ):
            errors.add(f"{trial}: stdout/CSV count mismatch for {role}/{worker}")
        if total < measured:
            errors.add(f"{trial}: total I/O count below measured count for {role}/{worker}")
        if max_gate_wait >= GATE_DEADLINE_NS or status:
            errors.add(f"{trial}: worker failure for {role}/{worker}")
        expected_uid = foreground_uid if role == "foreground_read" else 0
        if uid != expected_uid:
            errors.add(f"{trial}: stdout UID mismatch for {role}/{worker}")

    stats_delta = {
        name: 0 for name in (
            "fg_dispatched", "bg_dispatched", "depth_anomalies",
            "allocation_failures", "inference", "timeout", "invalid",
        )
    }
    stats_before_path = input_dir / f"{trial}-stats-before.txt"
    stats_after_path = input_dir / f"{trial}-stats-after.txt"
    if not stats_before_path.is_file() or not stats_after_path.is_file():
        errors.add(f"{trial}: missing module stats")
    else:
        try:
            stats_before = parse_stats(stats_before_path)
            stats_after = parse_stats(stats_after_path)
            stats_delta = {
                name: stats_after[name] - stats_before[name]
                for name in stats_delta
            }
            if stats_delta["fg_dispatched"] <= 0:
                errors.add(f"{trial}: no foreground dispatches classified")
            if stats_delta["bg_dispatched"] <= 0:
                errors.add(f"{trial}: no background dispatches classified")
            for name in (
                "depth_anomalies", "allocation_failures", "inference",
                "timeout", "invalid",
            ):
                if stats_delta[name]:
                    errors.add(f"{trial}: module {name} delta={stats_delta[name]}")
            for name in (
                "current_depth", "reserved_depth", "issued_depth", "meta_inuse",
            ):
                if stats_before[name]:
                    errors.add(f"{trial}: non-idle module start {name}={stats_before[name]}")
                if stats_after[name]:
                    errors.add(f"{trial}: unsettled module {name}={stats_after[name]}")
        except ValueError as error:
            errors.add(str(error))

    temperatures: dict[str, int] = {}
    for phase in ("before", "after"):
        path = input_dir / f"{trial}-{phase}-thermal.txt"
        if not path.is_file():
            errors.add(f"{trial}: missing {phase} thermal data")
            temperatures[phase] = -1
            continue
        try:
            temperatures[phase] = parse_battery_temp(path)
        except ValueError as error:
            errors.add(str(error))
            temperatures[phase] = -1
    if temperatures["before"] >= 420:
        errors.add(f"{trial}: started above thermal gate ({temperatures['before']})")
    if temperatures["after"] >= 450:
        errors.add(f"{trial}: ended at hard thermal stop ({temperatures['after']})")

    result.update({
        "battery_temp_before_tenths_c": temperatures["before"],
        "battery_temp_after_tenths_c": temperatures["after"],
        "fg_dispatch_delta": stats_delta["fg_dispatched"],
        "bg_dispatch_delta": stats_delta["bg_dispatched"],
        "pre_read_max_concurrency": concurrency[("pre", "background_read")],
        "pre_write_max_concurrency": concurrency[("pre", "background_write")],
        "read_max_concurrency": concurrency[("treatment", "background_read")],
        "write_max_concurrency": concurrency[("treatment", "background_write")],
        "read_limit": PROFILE_LIMITS[profile][0],
        "write_limit": PROFILE_LIMITS[profile][1],
    })
    for phase, seconds in (("pre", PRE_SECONDS), ("treatment", TREATMENT_SECONDS)):
        for role in ROLES:
            values = latencies[phase][role]
            waits = gate_waits[phase][role]
            if not values:
                continue
            prefix = ROLE_PREFIX[role]
            if phase == "pre":
                prefix = f"pre_{prefix}"
            add_distribution(result, prefix, values, waits, seconds)
    if "bg_read_iops" in result and "bg_write_iops" in result:
        result["bg_total_iops"] = (
            float(result["bg_read_iops"]) + float(result["bg_write_iops"])
        )
    if "pre_bg_read_iops" in result and "pre_bg_write_iops" in result:
        result["pre_bg_total_iops"] = (
            float(result["pre_bg_read_iops"])
            + float(result["pre_bg_write_iops"])
        )
    if all(
        name in result
        for name in ("pre_fg_p99_ns", "pre_bg_read_p99_ns", "pre_bg_write_p99_ns")
    ):
        result["pre_pressure"] = max(
            float(result["pre_fg_p99_ns"]) / 1_000_000.0,
            float(result["pre_bg_read_p99_ns"]) / 4_000_000.0,
            float(result["pre_bg_write_p99_ns"]) / 10_000_000.0,
        )
    result["quality_error_count"] = len(errors)
    return result, sorted(errors)


def profile_summary(
    trials: list[dict[str, int | float | str]],
) -> list[dict[str, int | float | str]]:
    rows = []
    for profile in PROFILES:
        selected = [trial for trial in trials if trial["profile"] == profile]
        row: dict[str, int | float | str] = {
            "profile": profile,
            "trials": len(selected),
        }
        for metric in METRICS:
            values = [float(trial[metric]) for trial in selected]
            row[f"{metric}_mean"] = statistics.fmean(values)
            row[f"{metric}_median"] = statistics.median(values)
        rows.append(row)
    return rows


def state_balance(
    trials: list[dict[str, int | float | str]],
    threshold: float,
) -> list[dict[str, int | float | str]]:
    rows = []
    state_metrics = (
        "pre_pressure", "pre_fg_p99_ns", "pre_bg_read_p99_ns",
        "pre_bg_write_p99_ns", "pre_fg_iops", "pre_bg_read_iops",
        "pre_bg_write_iops",
    )
    for profile in PROFILES:
        selected = [trial for trial in trials if trial["profile"] == profile]
        pressures = [float(trial["pre_pressure"]) for trial in selected]
        row: dict[str, int | float | str] = {
            "profile": profile,
            "trials": len(selected),
            "low_count": sum(value < threshold for value in pressures),
            "high_count": sum(value >= threshold for value in pressures),
            "median_threshold": threshold,
            "pre_pressure_sd": statistics.stdev(pressures) if len(pressures) > 1 else 0.0,
            "pre_pressure_min": min(pressures),
            "pre_pressure_max": max(pressures),
        }
        for metric in state_metrics:
            values = [float(trial[metric]) for trial in selected]
            row[f"{metric}_mean"] = statistics.fmean(values)
            row[f"{metric}_median"] = statistics.median(values)
        rows.append(row)
    return rows


def make_bootstrap_draws(
    block_ids: list[int], iterations: int, seed: int,
) -> list[tuple[int, ...]]:
    rng = random.Random(seed)
    return [
        tuple(rng.choice(block_ids) for _ in block_ids)
        for _ in range(iterations)
    ]


def unconditional_effects(
    trials: list[dict[str, int | float | str]],
    draws: list[tuple[int, ...]],
) -> list[dict[str, int | float | str | None]]:
    by_block: dict[int, dict[str, dict[str, int | float | str]]] = defaultdict(dict)
    for trial in trials:
        by_block[int(trial["block"])][str(trial["profile"])] = trial
    contrast_functions: dict[str, Callable[[dict[str, float]], float]] = {
        "READ_GUARD_vs_BASELINE": lambda p: p["READ_GUARD"] - p["BASELINE"],
        "WRITE_GUARD_vs_BASELINE": lambda p: p["WRITE_GUARD"] - p["BASELINE"],
        "ALL_GUARD_vs_BASELINE": lambda p: p["ALL_GUARD"] - p["BASELINE"],
        "read_guard_main_effect": lambda p: (
            p["READ_GUARD"] - p["BASELINE"]
            + p["ALL_GUARD"] - p["WRITE_GUARD"]
        ) / 2.0,
        "write_guard_main_effect": lambda p: (
            p["WRITE_GUARD"] - p["BASELINE"]
            + p["ALL_GUARD"] - p["READ_GUARD"]
        ) / 2.0,
        "guard_interaction": lambda p: (
            p["ALL_GUARD"] - p["READ_GUARD"]
            - p["WRITE_GUARD"] + p["BASELINE"]
        ),
    }
    output: list[dict[str, int | float | str | None]] = []
    for metric in METRICS:
        block_profiles = {
            block: {
                profile: float(by_block[block][profile][metric])
                for profile in PROFILES
            }
            for block in by_block
        }
        for contrast, function in contrast_functions.items():
            block_values = {
                block: function(values) for block, values in block_profiles.items()
            }
            observed = [block_values[block] for block in sorted(block_values)]
            bootstrap_values = [
                statistics.fmean(block_values[block] for block in draw)
                for draw in draws
            ]
            low, high = percentile_ci(bootstrap_values)
            output.append({
                "metric": metric,
                "contrast": contrast,
                "blocks": len(observed),
                "mean_effect": statistics.fmean(observed),
                "median_effect": statistics.median(observed),
                "ci95_low": low,
                "ci95_high": high,
                "negative_blocks": sum(value < 0 for value in observed),
                "positive_blocks": sum(value > 0 for value in observed),
                "zero_blocks": sum(value == 0 for value in observed),
                "valid_bootstrap_replicates": len(bootstrap_values),
            })
    return output


def conditional_effects(
    trials: list[dict[str, int | float | str]],
    draws: list[tuple[int, ...]],
    state_field: str,
    stratification: str,
    threshold: float,
) -> list[dict[str, int | float | str | None]]:
    by_block: dict[int, list[dict[str, int | float | str]]] = defaultdict(list)
    for trial in trials:
        by_block[int(trial["block"])].append(trial)
    output: list[dict[str, int | float | str | None]] = []
    for metric in METRICS:
        cells: dict[int, dict[tuple[str, str], tuple[float, int]]] = {}
        for block, block_trials in by_block.items():
            block_cells: dict[tuple[str, str], tuple[float, int]] = {}
            for trial in block_trials:
                key = (str(trial["profile"]), str(trial[state_field]))
                block_cells[key] = (float(trial[metric]), 1)
            cells[block] = block_cells

        def aggregate(profile: str, state: str, draw: tuple[int, ...]) -> tuple[float, int]:
            total = 0.0
            count = 0
            for block in draw:
                value = cells[block].get((profile, state))
                if value is not None:
                    total += value[0]
                    count += value[1]
            return total, count

        observed_draw = tuple(sorted(cells))
        for profile in PROFILES[1:]:
            observed: dict[str, tuple[float, float, int, int] | None] = {}
            for state in ("LOW", "HIGH"):
                action_sum, action_count = aggregate(profile, state, observed_draw)
                base_sum, base_count = aggregate("BASELINE", state, observed_draw)
                if action_count and base_count:
                    action_mean = action_sum / action_count
                    base_mean = base_sum / base_count
                    observed[state] = (
                        action_mean, base_mean, action_count, base_count
                    )
                else:
                    observed[state] = None

            bootstrap_by_estimand: dict[str, list[float]] = {
                "LOW": [], "HIGH": [], "HIGH_MINUS_LOW": [],
            }
            for draw in draws:
                effects: dict[str, float] = {}
                for state in ("LOW", "HIGH"):
                    action_sum, action_count = aggregate(profile, state, draw)
                    base_sum, base_count = aggregate("BASELINE", state, draw)
                    if action_count and base_count:
                        effects[state] = (
                            action_sum / action_count - base_sum / base_count
                        )
                        bootstrap_by_estimand[state].append(effects[state])
                if "LOW" in effects and "HIGH" in effects:
                    bootstrap_by_estimand["HIGH_MINUS_LOW"].append(
                        effects["HIGH"] - effects["LOW"]
                    )

            for estimand in ("LOW", "HIGH", "HIGH_MINUS_LOW"):
                low_observed = observed["LOW"]
                high_observed = observed["HIGH"]
                if estimand == "LOW":
                    selected = low_observed
                    effect = (
                        selected[0] - selected[1] if selected is not None else None
                    )
                    action_mean = selected[0] if selected is not None else None
                    baseline_mean = selected[1] if selected is not None else None
                elif estimand == "HIGH":
                    selected = high_observed
                    effect = (
                        selected[0] - selected[1] if selected is not None else None
                    )
                    action_mean = selected[0] if selected is not None else None
                    baseline_mean = selected[1] if selected is not None else None
                else:
                    selected = None
                    effect = None
                    if low_observed is not None and high_observed is not None:
                        effect = (
                            high_observed[0] - high_observed[1]
                            - low_observed[0] + low_observed[1]
                        )
                    action_mean = None
                    baseline_mean = None
                bootstrap_values = bootstrap_by_estimand[estimand]
                ci_low, ci_high = percentile_ci(bootstrap_values)
                output.append({
                    "stratification": stratification,
                    "threshold": threshold,
                    "metric": metric,
                    "profile": profile,
                    "estimand": estimand,
                    "action_low_n": low_observed[2] if low_observed is not None else 0,
                    "baseline_low_n": low_observed[3] if low_observed is not None else 0,
                    "action_high_n": high_observed[2] if high_observed is not None else 0,
                    "baseline_high_n": high_observed[3] if high_observed is not None else 0,
                    "action_mean": action_mean,
                    "baseline_mean": baseline_mean,
                    "mean_effect": effect,
                    "ci95_low": ci_low,
                    "ci95_high": ci_high,
                    "valid_bootstrap_replicates": len(bootstrap_values),
                })
    return output


def candidate_assessments(
    balance: list[dict[str, int | float | str]],
    conditional: list[dict[str, int | float | str | None]],
    iterations: int,
    quality_ok: bool,
) -> list[dict[str, int | float | str | bool | None]]:
    cells_ok = all(
        int(row["low_count"]) >= 8 and int(row["high_count"]) >= 8
        for row in balance
    )
    lookup = {
        (str(row["profile"]), str(row["estimand"])): row
        for row in conditional
        if row["stratification"] == "median_pre_pressure"
        and row["metric"] == "fg_p99_ns"
    }
    output = []
    for profile in PROFILES[1:]:
        high = lookup.get((profile, "HIGH"), {})
        modifier = lookup.get((profile, "HIGH_MINUS_LOW"), {})
        high_ci_upper = high.get("ci95_high")
        modifier_ci_low = modifier.get("ci95_low")
        modifier_ci_high = modifier.get("ci95_high")
        valid_high = int(high.get("valid_bootstrap_replicates", 0))
        valid_modifier = int(modifier.get("valid_bootstrap_replicates", 0))
        bootstrap_ok = (
            valid_high >= math.ceil(iterations * BOOTSTRAP_MIN_VALID_RATIO)
            and valid_modifier >= math.ceil(iterations * BOOTSTRAP_MIN_VALID_RATIO)
        )
        modifier_excludes_zero = (
            modifier_ci_low is not None
            and modifier_ci_high is not None
            and (float(modifier_ci_low) > 0 or float(modifier_ci_high) < 0)
        )
        high_benefit = high_ci_upper is not None and float(high_ci_upper) < 0
        supported = (
            quality_ok and cells_ok and bootstrap_ok
            and modifier_excludes_zero and high_benefit
        )
        output.append({
            "profile": profile,
            "quality_gate": quality_ok,
            "cell_count_gate": cells_ok,
            "bootstrap_validity_gate": bootstrap_ok,
            "modifier_nonzero_gate": modifier_excludes_zero,
            "high_benefit_gate": high_benefit,
            "supported": supported,
            "high_effect_ns": high.get("mean_effect"),
            "high_ci95_low_ns": high.get("ci95_low"),
            "high_ci95_high_ns": high_ci_upper,
            "modifier_ns": modifier.get("mean_effect"),
            "modifier_ci95_low_ns": modifier_ci_low,
            "modifier_ci95_high_ns": modifier_ci_high,
        })
    return output


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="ascii") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def fmt_ns(value: object) -> str:
    if value is None:
        return "N/A"
    return f"{float(value) / 1000.0:,.1f} us"


def build_report(
    schedule_path: Path,
    pilot: bool,
    trials: list[dict[str, int | float | str]],
    profiles: list[dict[str, int | float | str]],
    balance: list[dict[str, int | float | str]],
    threshold: float | None,
    unconditional: list[dict[str, int | float | str | None]],
    conditional: list[dict[str, int | float | str | None]],
    assessments: list[dict[str, int | float | str | bool | None]],
    errors: list[str],
) -> str:
    title = "Treatment 前状态条件化 Guard pilot 报告" if pilot else (
        "Treatment 前状态条件化 Guard 随机实验报告"
    )
    lines = [
        f"# {title}",
        "",
        "## 有效性",
        "",
        f"- 随机表：{schedule_path.name}。",
        f"- 完成 trial：{len(trials)}；完整 block：{len(trials) // 4}。",
        f"- 数据质量错误：{len(errors)}。",
        "- State 只使用 Action 生效前已经完成的 I/O；独立实验单元是 block。",
    ]
    if threshold is not None:
        threshold_kind = "pilot 描述性" if pilot else "正式"
        lines.append(
            f"- {threshold_kind}中位数 pre_pressure 阈值：{threshold:.6f}。"
        )
    lines.extend([
        "",
        "## State 平衡",
        "",
        "| Profile | LOW | HIGH | pre_pressure 均值 | pre_pressure 中位数 |",
        "| --- | ---: | ---: | ---: | ---: |",
    ])
    for row in balance:
        lines.append(
            f"| {row['profile']} | {row['low_count']} | {row['high_count']} | "
            f"{float(row['pre_pressure_mean']):.4f} | "
            f"{float(row['pre_pressure_median']):.4f} |"
        )
    lines.extend([
        "",
        "## Treatment 汇总",
        "",
        "| Profile | 前台 READ P99 | 前台慢比例 | 前台 IOPS | 后台 READ IOPS | 后台 WRITE IOPS |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ])
    for row in profiles:
        lines.append(
            f"| {row['profile']} | {fmt_ns(row['fg_p99_ns_mean'])} | "
            f"{100.0 * float(row['fg_slow_ratio_mean']):.2f}% | "
            f"{float(row['fg_iops_mean']):,.1f} | "
            f"{float(row['bg_read_iops_mean']):,.1f} | "
            f"{float(row['bg_write_iops_mean']):,.1f} |"
        )

    if pilot:
        lines.extend([
            "",
            "## 判定",
            "",
            "pilot 只检查 phase 隔离、并发、身份、模块状态和数据完整性，不估计 "
            "HIGH/LOW 条件效应，也不形成策略结论。",
        ])
    else:
        lines.extend([
            "",
            "## 无条件前台 P99 效应",
            "",
            "负值表示 Guard 降低前台 READ P99；区间按完整 block 重采样。",
            "",
            "| Contrast | 均值效应 | 95% CI |",
            "| --- | ---: | ---: |",
        ])
        for row in unconditional:
            if row["metric"] == "fg_p99_ns" and "_vs_BASELINE" in str(row["contrast"]):
                lines.append(
                    f"| {row['contrast']} | {fmt_ns(row['mean_effect'])} | "
                    f"[{fmt_ns(row['ci95_low'])}, {fmt_ns(row['ci95_high'])}] |"
                )
        lines.extend([
            "",
            "## 中位数状态条件下的前台 P99 效应",
            "",
            "`HIGH_MINUS_LOW` 是条件处理效应之差，不是两组原始延迟之差。",
            "",
            "| Profile | Estimand | 效应 | 95% CI | 有效 bootstrap |",
            "| --- | --- | ---: | ---: | ---: |",
        ])
        for row in conditional:
            if row["stratification"] == "median_pre_pressure" and row["metric"] == "fg_p99_ns":
                lines.append(
                    f"| {row['profile']} | {row['estimand']} | "
                    f"{fmt_ns(row['mean_effect'])} | "
                    f"[{fmt_ns(row['ci95_low'])}, {fmt_ns(row['ci95_high'])}] | "
                    f"{row['valid_bootstrap_replicates']} |"
                )
        lines.extend(["", "## 预注册判定", ""])
        supported = [row for row in assessments if bool(row["supported"])]
        if supported:
            lines.append("满足全部预注册门槛的候选 Action：")
            lines.extend(f"- {row['profile']}" for row in supported)
        else:
            lines.append(
                "没有 Action 满足全部预注册门槛；当前外部 pre-state 不足以支持"
                "状态触发策略。"
            )
        for row in assessments:
            lines.append(
                f"- {row['profile']}: cells={row['cell_count_gate']}, "
                f"bootstrap={row['bootstrap_validity_gate']}, "
                f"modifier={row['modifier_nonzero_gate']}, "
                f"high_benefit={row['high_benefit_gate']}。"
            )

    if errors:
        lines.extend(["", "## 数据质量错误", ""])
        lines.extend(f"- {error}" for error in errors[:100])
    lines.extend([
        "",
        "本实验只验证用户态主机提交边界的 per-op Guard；不代表正式内核已经"
        "实现该 Action，也不识别 UFS/FTL 内部原因、闪存老化、冷启动或 FPS。",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule", type=Path, required=True)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--bootstrap", type=int, default=20000)
    parser.add_argument("--seed", type=int, default=2026080106)
    parser.add_argument("--pilot", action="store_true")
    args = parser.parse_args()
    if args.bootstrap < 1000:
        raise ValueError("bootstrap iterations must be at least 1000")

    schedule = load_schedule(args.schedule)
    config, foreground_uid = parse_config(args.input_dir)
    errors = validate_design(schedule, args.pilot)
    errors.extend(validate_run_files(args.input_dir, args.schedule, config))
    parsed_trials = []
    trial_errors: dict[str, list[str]] = {}
    for expected in schedule:
        trial, current_errors = parse_trial(args.input_dir, expected, foreground_uid)
        parsed_trials.append(trial)
        if current_errors:
            trial_errors[str(trial["trial"])] = current_errors
            errors.extend(current_errors)
    complete_trials = [
        trial for trial in parsed_trials
        if all(metric in trial for metric in METRICS)
        and "pre_pressure" in trial
    ]
    if len(complete_trials) != len(schedule):
        errors.append(
            f"complete trials {len(complete_trials)} != scheduled {len(schedule)}"
        )
    errors = sorted(set(errors))

    threshold: float | None = None
    balance: list[dict[str, int | float | str]] = []
    profiles: list[dict[str, int | float | str]] = []
    if complete_trials:
        threshold = statistics.median(
            float(trial["pre_pressure"]) for trial in complete_trials
        )
        for trial in complete_trials:
            trial["median_state"] = (
                "HIGH" if float(trial["pre_pressure"]) >= threshold else "LOW"
            )
            trial["absolute_state"] = (
                "HIGH" if float(trial["pre_pressure"]) >= 1.0 else "LOW"
            )
        balance = state_balance(complete_trials, threshold)
        if all(sum(trial["profile"] == profile for trial in complete_trials) for profile in PROFILES):
            profiles = profile_summary(complete_trials)

    unconditional: list[dict[str, int | float | str | None]] = []
    conditional: list[dict[str, int | float | str | None]] = []
    assessments: list[dict[str, int | float | str | bool | None]] = []
    formal_complete = (
        not args.pilot and len(complete_trials) == len(schedule)
        and len(schedule) == 96 and threshold is not None
    )
    if formal_complete:
        block_ids = sorted({int(trial["block"]) for trial in complete_trials})
        draws = make_bootstrap_draws(block_ids, args.bootstrap, args.seed)
        unconditional = unconditional_effects(complete_trials, draws)
        conditional.extend(conditional_effects(
            complete_trials, draws, "median_state", "median_pre_pressure", threshold
        ))
        conditional.extend(conditional_effects(
            complete_trials, draws, "absolute_state", "absolute_pre_pressure_1", 1.0
        ))
        assessments = candidate_assessments(
            balance, conditional, args.bootstrap, not errors
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.output_dir / "trial-summary.csv", complete_trials)
    write_csv(args.output_dir / "state-balance.csv", balance)
    write_csv(args.output_dir / "profile-summary.csv", profiles)
    write_csv(args.output_dir / "unconditional-effects.csv", unconditional)
    write_csv(args.output_dir / "conditional-effects.csv", conditional)
    summary = {
        "mode": "pilot" if args.pilot else "formal",
        "schedule": str(args.schedule),
        "schedule_sha256": sha256_file(args.schedule),
        "scheduled_trials": len(schedule),
        "complete_trials": len(complete_trials),
        "blocks": len(schedule) // 4,
        "bootstrap_iterations": 0 if args.pilot else args.bootstrap,
        "bootstrap_seed": None if args.pilot else args.seed,
        "foreground_uid": foreground_uid,
        "median_pre_pressure_threshold": threshold,
        "data_quality_errors": errors,
        "trial_errors": trial_errors,
        "state_balance": balance,
        "profiles": profiles,
        "candidate_assessments": assessments,
        "supported_candidates": [
            row["profile"] for row in assessments if bool(row["supported"])
        ],
        "unconditional_effects": unconditional,
        "conditional_effects": conditional,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=True, allow_nan=False) + "\n",
        encoding="ascii",
    )
    report = build_report(
        args.schedule, args.pilot, complete_trials, profiles, balance, threshold,
        unconditional, conditional, assessments, errors,
    )
    (args.output_dir / "report.md").write_text(report, encoding="utf-8")
    print(
        f"mode={'pilot' if args.pilot else 'formal'} scheduled={len(schedule)} "
        f"complete={len(complete_trials)} errors={len(errors)} "
        f"output={args.output_dir}"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
