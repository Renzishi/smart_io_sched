#!/usr/bin/env python3
"""Compare Android kernel joint-model results with locked local golden vectors."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np


VECTOR_MAGIC = 0x534A5631
RESULT_MAGIC = 0x534A5231
VERSION = 1
HEADER = struct.Struct("<IIII")
VECTOR_DTYPE = np.dtype(
    [
        ("row_id", "<u8"),
        ("input_bits", "<u4", (11,)),
        ("expected_action", "<u4"),
        ("expected_q_bits", "<u4", (12,)),
    ]
)
RESULT_DTYPE = np.dtype(
    [("row_id", "<u8"), ("kernel_action", "<u4"), ("kernel_q_bits", "<u4", (12,))]
)


def read_header(stream, expected_magic: int, expected_record_size: int) -> int:
    magic, version, record_size, count = HEADER.unpack(stream.read(HEADER.size))
    if (magic, version, record_size) != (expected_magic, VERSION, expected_record_size):
        raise ValueError("invalid vector/result header")
    return count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vectors", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--q-atol", type=float, default=1e-5)
    parser.add_argument("--batch-size", type=int, default=4096)
    args = parser.parse_args()

    total = 0
    mismatches = 0
    robust_mismatches = 0
    max_abs_error = 0.0
    expected_counts = np.zeros(12, dtype=np.uint64)
    kernel_counts = np.zeros(12, dtype=np.uint64)
    with args.vectors.open("rb") as vectors, args.results.open("rb") as results:
        vector_count = read_header(vectors, VECTOR_MAGIC, VECTOR_DTYPE.itemsize)
        result_count = read_header(results, RESULT_MAGIC, RESULT_DTYPE.itemsize)
        if vector_count != result_count:
            raise ValueError("vector/result count mismatch")
        while total < vector_count:
            count = min(args.batch_size, vector_count - total)
            expected = np.fromfile(vectors, dtype=VECTOR_DTYPE, count=count)
            observed = np.fromfile(results, dtype=RESULT_DTYPE, count=count)
            if len(expected) != count or len(observed) != count:
                raise ValueError("truncated vector/result data")
            if not np.array_equal(expected["row_id"], observed["row_id"]):
                raise ValueError("row id mismatch")
            expected_action = expected["expected_action"]
            kernel_action = observed["kernel_action"]
            if np.any(kernel_action >= 12):
                raise ValueError("kernel returned invalid action")
            expected_counts += np.bincount(expected_action, minlength=12).astype(np.uint64)
            kernel_counts += np.bincount(kernel_action, minlength=12).astype(np.uint64)
            expected_q = expected["expected_q_bits"].view(np.float32).reshape(count, 12)
            kernel_q = observed["kernel_q_bits"].view(np.float32).reshape(count, 12)
            errors = np.abs(expected_q - kernel_q)
            max_abs_error = max(max_abs_error, float(errors.max()))
            different = expected_action != kernel_action
            mismatches += int(different.sum())
            sorted_q = np.partition(expected_q, -2, axis=1)
            margins = sorted_q[:, -1] - sorted_q[:, -2]
            robust_mismatches += int((different & (margins > 2.0 * args.q_atol)).sum())
            total += count

    summary = {
        "samples": total,
        "action_mismatches": mismatches,
        "action_match_rate": 1.0 - mismatches / total,
        "robust_action_mismatches": robust_mismatches,
        "max_abs_q_error": max_abs_error,
        "q_atol": args.q_atol,
        "expected_action_counts": expected_counts.tolist(),
        "kernel_action_counts": kernel_counts.tolist(),
        "passed": mismatches == 0 and max_abs_error <= args.q_atol,
    }
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
