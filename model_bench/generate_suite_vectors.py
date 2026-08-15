#!/usr/bin/env python3
"""Generate full-dataset expected FP32 and INT8 Actions for model 0."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np

from generate_model_suite import (
    MULT_SHIFT,
    actual_weights,
    forward,
    load_inputs,
    quantize,
)


MAGIC = 0x53515631


def int8_forward(inputs: np.ndarray, layers, qparams) -> np.ndarray:
    scaled = inputs * 20.0
    current = np.where(
        scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5)
    ).clip(-127, 127).astype(np.int8)
    for layer, (weights, bias) in enumerate(
        zip(qparams["weights"], qparams["biases"])
    ):
        accum = current.astype(np.int32) @ weights.astype(np.int32).T
        accum += bias.astype(np.int32)
        product = accum.astype(np.int64) * qparams["multipliers"][layer].astype(
            np.int64
        )
        values = np.where(
            product >= 0,
            (product + (1 << (MULT_SHIFT - 1))) >> MULT_SHIFT,
            -((-product + (1 << (MULT_SHIFT - 1))) >> MULT_SHIFT),
        )
        if layer + 1 != len(layers):
            current = values.clip(0, 127).astype(np.int8)
        else:
            return values.astype(np.int32)
    raise AssertionError("model has no output layer")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    spec = json.loads((args.model_dir / "model_spec.json").read_text(encoding="utf-8"))
    states, _, _, normalized = load_inputs(args.dataset, spec)
    layers = actual_weights(args.model_dir / "weights_fp32.npz")
    qparams = quantize(layers, normalized)
    fp32_q, _ = forward(normalized, layers)
    fp32_action = np.argmax(fp32_q, axis=1).astype(np.uint32)
    int8_q = int8_forward(normalized, layers, qparams)
    int8_action = np.argmax(int8_q, axis=1).astype(np.uint32)
    scaled = normalized * 20.0
    int8_input = np.where(
        scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5)
    ).clip(-127, 127).astype(np.int8)

    with args.output.open("wb") as stream:
        stream.write(struct.pack("<IIII", MAGIC, 1, 0, len(states)))
        for index, (state, fp_action, int_action) in enumerate(
            zip(states.astype(np.float32), fp32_action, int8_action)
        ):
            stream.write(state.astype("<f4").tobytes())
            stream.write(int8_input[index].tobytes())
            stream.write(b"\0")
            stream.write(struct.pack("<II", int(fp_action), int(int_action)))
    margins = np.partition(fp32_q, -2, axis=1)[:, -1] - np.partition(
        fp32_q, -2, axis=1
    )[:, -2]
    robust = margins > 2e-5
    summary = {
        "count": int(len(states)),
        "model_id": 0,
        "fp32_vs_int8_action_mismatches": int(
            np.count_nonzero(fp32_action != int8_action)
        ),
        "fp32_vs_int8_match_rate": float(np.mean(fp32_action == int8_action)),
        "robust_count": int(np.count_nonzero(robust)),
        "robust_mismatches": int(
            np.count_nonzero((fp32_action != int8_action) & robust)
        ),
        "min_fp32_margin": float(np.min(margins)),
        "int8_q_range": [int(np.min(int8_q)), int(np.max(int8_q))],
    }
    args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
