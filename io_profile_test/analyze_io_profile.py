#!/usr/bin/env python3
"""Analyze bounded probe CSV files and selected smart_io raw trace lines."""
import argparse
import csv
import glob
import json
import math
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    index = int((len(values) - 1) * q)
    return values[index]


def summarize(values):
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "mean_ns": statistics.fmean(values),
        "p50_ns": percentile(values, 0.50),
        "p95_ns": percentile(values, 0.95),
        "p99_ns": percentile(values, 0.99),
        "max_ns": max(values),
        "slow_ge_4ms": sum(v >= 4_000_000 for v in values),
        "slow_ge_4ms_ratio": sum(v >= 4_000_000 for v in values) / len(values),
    }


def read_probe(paths):
    groups = defaultdict(list)
    errors = Counter()
    for pattern in paths:
        for filename in sorted(glob.glob(pattern)):
            with open(filename, newline="") as stream:
                for row in csv.DictReader(stream):
                    try:
                        latency = int(row["latency_ns"])
                        result = int(row["ret"])
                    except (KeyError, ValueError):
                        errors["malformed_row"] += 1
                        continue
                    if result < 0:
                        errors[row.get("errno", "unknown")] += 1
                        continue
                    groups[(Path(filename).stem, row.get("op", "unknown"),
                            row.get("ioprio_class", "unknown"))].append(latency)
    return {"groups": {"|".join(key): summarize(value)
                       for key, value in sorted(groups.items())},
            "errors": dict(errors)}


TOKEN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^ ]+)")


def parse_tokens(line):
    return {key: value for key, value in TOKEN_RE.findall(line)}


def ioprio_is_rt(text):
    try:
        return (int(text) >> 13) == 1
    except (TypeError, ValueError):
        return False


def op_name(value):
    return {"0": "read", "1": "write", "2": "fsync",
            "3": "discard"}.get(value, value or "unknown")


def read_trace(paths):
    counts = Counter()
    nonrt_candidates = Counter()
    fg_candidates_by_uid_op = Counter()
    nonrt_candidates_by_uid_op = Counter()
    completion_latencies = defaultdict(list)
    completion_by_class = defaultdict(list)
    request_meta = {}
    issued_meta = {}
    headers = []
    for pattern in paths:
        for filename in sorted(glob.glob(pattern)):
            with open(filename, errors="replace") as stream:
                for line in stream:
                    if "entries-in-buffer/entries-written:" in line:
                        headers.append(line.strip())
                    if "block_rq_insert:" in line:
                        fields = parse_tokens(line)
                        rq = fields.get("rq_p")
                        counts["insert"] += 1
                        if rq:
                            request_meta[rq] = fields
                        continue
                    if "block_rq_issue:" not in line:
                        if "block_rq_complete:" in line:
                            fields = parse_tokens(line)
                            rq = fields.get("rq_p")
                            meta = issued_meta.get(rq, request_meta.get(rq, {}))
                            op = op_name(meta.get("io_op", fields.get("io_op")))
                            try:
                                latency = int(fields.get("dev_lat_ns", "0"))
                            except ValueError:
                                counts["bad_completion_latency"] += 1
                                continue
                            counts["complete"] += 1
                            completion_latencies[op].append(latency)
                            if meta:
                                counts["complete_with_metadata"] += 1
                                is_rt = ioprio_is_rt(meta.get("ioprio"))
                                is_fg = meta.get("fg") == "1"
                                if is_fg and not is_rt:
                                    cls = "fg_nonrt"
                                elif is_rt:
                                    cls = "rt"
                                elif is_fg:
                                    cls = "fg_other"
                                else:
                                    cls = "other"
                                completion_by_class[(cls, op)].append(latency)
                            else:
                                counts["complete_without_metadata"] += 1
                        continue
                    fields = parse_tokens(line)
                    rq = fields.get("rq_p")
                    meta = dict(request_meta.get(rq, {}))
                    meta.update(fields)
                    if rq:
                        issued_meta[rq] = meta
                    counts["issue"] += 1
                    op = op_name(meta.get("io_op"))
                    counts[f"issue_op_{op}"] += 1
                    if request_meta.get(rq):
                        counts["issue_with_insert_metadata"] += 1
                    else:
                        counts["issue_without_insert_metadata"] += 1
                    is_rt = ioprio_is_rt(meta.get("ioprio"))
                    if meta.get("fg") == "1":
                        counts["fg_uid_match"] += 1
                        uid = meta.get("uid", "unknown")
                        fg_candidates_by_uid_op[(uid, op)] += 1
                        if not is_rt:
                            nonrt_candidates[op] += 1
                            nonrt_candidates_by_uid_op[(uid, op)] += 1
                    if is_rt:
                        counts["rt"] += 1
                        if op == "read":
                            counts["rt_read"] += 1
    return {
        "headers": headers,
        "counts": dict(counts),
        "observed_nonrt_fg_candidates_by_op": dict(nonrt_candidates),
        "fg_candidates_by_uid_and_op": {
            "|".join(key): value
            for key, value in sorted(fg_candidates_by_uid_op.items())
        },
        "observed_nonrt_fg_candidates_by_uid_and_op": {
            "|".join(key): value
            for key, value in sorted(nonrt_candidates_by_uid_op.items())
        },
        "completion_latency_by_op": {
            op: summarize(values) for op, values in sorted(completion_latencies.items())
        },
        "completion_latency_by_class_and_op": {
            "|".join(key): summarize(values)
            for key, values in sorted(completion_by_class.items())
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", action="append", default=[])
    parser.add_argument("--trace", action="append", default=[])
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    result = {"probe": read_probe(args.probe),
              "trace": read_trace(args.trace) if args.trace else {}}
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
