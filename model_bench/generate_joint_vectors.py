#!/usr/bin/env python3
"""Generate all-State FP32 joint-action golden vectors for kernel validation."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
import polars as pl


MAGIC = 0x534A5631
VERSION = 1
VECTOR_DTYPE = np.dtype(
    [
        ("row_id", "<u8"),
        ("input_bits", "<u4", (11,)),
        ("expected_action", "<u4"),
        ("expected_q_bits", "<u4", (12,)),
    ]
)
HEADER = struct.Struct("<IIII")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_model(model_spec: Path, weights_path: Path) -> tuple[dict, dict[str, np.ndarray]]:
    spec = json.loads(model_spec.read_text(encoding="utf-8"))
    architecture = spec["architecture"]
    if (
        architecture.get("type") != "joint"
        or architecture.get("state_dim") != 11
        or architecture.get("hidden_dim") != 32
        or architecture.get("action_count") != 12
    ):
        raise ValueError(f"unexpected architecture: {architecture}")
    expected_shapes = {
        "trunk.0.weight": (32, 11),
        "trunk.0.bias": (32,),
        "trunk.2.weight": (32, 32),
        "trunk.2.bias": (32,),
        "joint_head.weight": (12, 32),
        "joint_head.bias": (12,),
    }
    with np.load(weights_path) as archive:
        weights = {
            name: archive[name].astype(np.float32, copy=False)
            for name in expected_shapes
        }
    for name, shape in expected_shapes.items():
        if weights[name].shape != shape or not np.isfinite(weights[name]).all():
            raise ValueError(f"invalid {name}: {weights[name].shape}")
    return spec, weights


def predict(states: np.ndarray, spec: dict, weights: dict[str, np.ndarray]) -> np.ndarray:
    scaler = spec["preprocessing"]
    mean = np.asarray(scaler["mean"], dtype=np.float32)
    std = np.asarray(scaler["std"], dtype=np.float32)
    clip = np.float32(scaler["clip"])
    normalized = np.clip((states - mean) / std, -clip, clip)
    hidden = np.maximum(
        normalized @ weights["trunk.0.weight"].T + weights["trunk.0.bias"], 0.0
    )
    hidden = np.maximum(
        hidden @ weights["trunk.2.weight"].T + weights["trunk.2.bias"], 0.0
    )
    return hidden @ weights["joint_head.weight"].T + weights["joint_head.bias"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-spec", type=Path, required=True)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=4096)
    args = parser.parse_args()
    if args.batch_size <= 0:
        raise ValueError("batch size must be positive")

    spec, weights = load_model(args.model_spec, args.weights)
    columns = spec["state_columns"]
    frame = pl.read_parquet(args.dataset, columns=columns)
    states = np.ascontiguousarray(frame.to_numpy(), dtype=np.float32)
    if states.ndim != 2 or states.shape[1] != 11 or not np.isfinite(states).all():
        raise ValueError(f"invalid State matrix: {states.shape}")

    action_counts = np.zeros(12, dtype=np.uint64)
    max_q_margin = 0.0
    min_q_margin = float("inf")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as output:
        output.write(HEADER.pack(MAGIC, VERSION, VECTOR_DTYPE.itemsize, len(states)))
        for start in range(0, len(states), args.batch_size):
            batch = states[start : start + args.batch_size]
            q_values = np.ascontiguousarray(predict(batch, spec, weights), dtype=np.float32)
            if q_values.shape != (len(batch), 12) or not np.isfinite(q_values).all():
                raise ValueError("invalid joint Q values")
            actions = q_values.argmax(axis=1).astype(np.uint32, copy=False)
            sorted_q = np.partition(q_values, -2, axis=1)
            margins = sorted_q[:, -1] - sorted_q[:, -2]
            max_q_margin = max(max_q_margin, float(margins.max()))
            min_q_margin = min(min_q_margin, float(margins.min()))
            action_counts += np.bincount(actions, minlength=12).astype(np.uint64)

            records = np.empty(len(batch), dtype=VECTOR_DTYPE)
            records["row_id"] = np.arange(start, start + len(batch), dtype=np.uint64)
            records["input_bits"] = batch.view("<u4")
            records["expected_action"] = actions
            records["expected_q_bits"] = q_values.view("<u4")
            records.tofile(output)

    manifest = {
        "format": {"magic": MAGIC, "version": VERSION, "record_size": VECTOR_DTYPE.itemsize},
        "count": len(states),
        "model_spec_sha256": sha256(args.model_spec),
        "weights_sha256": sha256(args.weights),
        "dataset_sha256": sha256(args.dataset),
        "vectors_sha256": sha256(args.output),
        "state_columns": columns,
        "expected_action_counts": action_counts.tolist(),
        "min_top1_top2_margin": min_q_margin,
        "max_top1_top2_margin": max_q_margin,
    }
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
