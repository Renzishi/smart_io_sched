#!/usr/bin/env python3
"""Generate reproducible FP32/INT8 parameters for the kernel model-suite bench."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq


INPUTS = 11
ACTIONS = 12
INPUT_Q_SCALE = 20.0
OUTPUT_Q_SCALE = 4096.0
MULT_SHIFT = 16
ARCHITECTURES = (
    ("actual_11_32_32_12", (11, 32, 32, 12), False),
    ("random_11_32_32_32_12", (11, 32, 32, 32, 12), True),
    ("random_11_64_12", (11, 64, 12), True),
    ("random_11_64_64_12", (11, 64, 64, 12), True),
    ("random_11_16_32_16_12", (11, 16, 32, 16, 12), True),
    ("random_11_64_64_64_12", (11, 64, 64, 64, 12), True),
    ("random_11_32_64_32_12", (11, 32, 64, 32, 12), True),
    ("random_11_64_128_64_12", (11, 64, 128, 64, 12), True),
)


def c_values(values: np.ndarray, c_type: str, per_line: int = 8) -> str:
    flat = values.reshape(-1)
    chunks = []
    for start in range(0, len(flat), per_line):
        row = flat[start : start + per_line]
        if c_type == "float":
            values = []
            for value in row:
                formatted = f"{float(value):.9g}"
                if "." not in formatted and "e" not in formatted:
                    formatted += ".0"
                values.append(formatted + "f")
            text = ", ".join(values)
        elif c_type == "s8":
            text = ", ".join(str(int(v)) for v in row)
        elif c_type == "s32":
            text = ", ".join(str(int(v)) for v in row)
        else:
            raise ValueError(c_type)
        chunks.append("\t" + text)
    return ",\n".join(chunks)


def load_inputs(dataset: Path, spec: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    cols = spec["state_columns"]
    table = pq.read_table(dataset, columns=cols)
    states = np.column_stack([table.column(c).to_numpy() for c in cols]).astype(np.float32)
    mean = np.asarray(spec["preprocessing"]["mean"], dtype=np.float32)
    std = np.asarray(spec["preprocessing"]["std"], dtype=np.float32)
    normalized = np.clip((states - mean) / std, -5.0, 5.0).astype(np.float32)
    return states, mean, std, normalized


def actual_weights(weights_path: Path) -> list[tuple[np.ndarray, np.ndarray]]:
    values = np.load(weights_path)
    return [
        (values["trunk.0.weight"], values["trunk.0.bias"]),
        (values["trunk.2.weight"], values["trunk.2.bias"]),
        (values["joint_head.weight"], values["joint_head.bias"]),
    ]


def random_weights(dims: tuple[int, ...], seed: int) -> list[tuple[np.ndarray, np.ndarray]]:
    rng = np.random.default_rng(seed)
    result = []
    for in_dim, out_dim in zip(dims[:-1], dims[1:]):
        weight = rng.normal(0.0, np.sqrt(2.0 / in_dim), size=(out_dim, in_dim))
        bias = rng.normal(0.0, 0.05, size=(out_dim,))
        result.append((weight.astype(np.float32), bias.astype(np.float32)))
    return result


def forward(inputs: np.ndarray, layers: list[tuple[np.ndarray, np.ndarray]]) -> tuple[np.ndarray, list[np.ndarray]]:
    current = inputs
    hidden = []
    for index, (weight, bias) in enumerate(layers):
        current = current @ weight.T + bias
        if index + 1 != len(layers):
            current = np.maximum(current, 0.0)
            hidden.append(current)
    return current.astype(np.float32), hidden


def round_half_away(values: np.ndarray) -> np.ndarray:
    return np.where(values >= 0.0, np.floor(values + 0.5), np.ceil(values - 0.5))


def quantize(layers: list[tuple[np.ndarray, np.ndarray]], calibration: np.ndarray) -> dict:
    _, activations = forward(calibration, layers)
    activation_scales = [INPUT_Q_SCALE]
    for activation in activations:
        maximum = max(float(np.max(activation)) * 1.01, 1e-4)
        activation_scales.append(np.float32(127.0 / maximum))
    activation_scales.append(OUTPUT_Q_SCALE)

    weights = []
    biases = []
    multipliers = []
    weight_scales = []
    for layer_index, (weight, bias) in enumerate(layers):
        maximum = np.maximum(np.max(np.abs(weight), axis=1), 1e-8)
        scale = (maximum / 127.0).astype(np.float32)
        quantized_weight = np.clip(round_half_away(weight / scale[:, None]), -127, 127).astype(np.int8)
        in_scale = float(activation_scales[layer_index])
        out_scale = float(activation_scales[layer_index + 1])
        quantized_bias = round_half_away(bias * in_scale / scale).astype(np.int32)
        multiplier = round_half_away(
            scale * out_scale / in_scale * float(1 << MULT_SHIFT)
        ).astype(np.int64)
        if np.max(np.abs(multiplier)) > np.iinfo(np.int32).max:
            raise ValueError("requantization multiplier exceeds int32")
        weights.append(quantized_weight)
        biases.append(quantized_bias)
        multipliers.append(multiplier.astype(np.int32))
        weight_scales.append(scale)
    return {
        "weights": weights,
        "biases": biases,
        "multipliers": multipliers,
        "weight_scales": weight_scales,
        "activation_scales": np.asarray(activation_scales, dtype=np.float32),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    spec = json.loads((args.model_dir / "model_spec.json").read_text(encoding="utf-8"))
    states, mean, std, normalized = load_inputs(args.dataset, spec)
    models = []
    header = [
        "/* SPDX-License-Identifier: GPL-2.0-only */",
        "/* Generated by generate_model_suite.py. Do not edit. */",
        "#ifndef SMART_IO_MODEL_SUITE_PARAMS_H",
        "#define SMART_IO_MODEL_SUITE_PARAMS_H",
        "",
        "#include <linux/types.h>",
        "",
        f"#define SMART_IO_SUITE_MODEL_COUNT {len(ARCHITECTURES)}U",
        "#define SMART_IO_SUITE_MAX_LAYERS 4U",
        "#define SMART_IO_SUITE_MAX_DIMS 5U",
        f"#define SMART_IO_SUITE_MULT_SHIFT {MULT_SHIFT}U",
        "#define SMART_IO_SUITE_INPUT_Q_SCALE 20.0f",
        "#define SMART_IO_SUITE_OUTPUT_Q_SCALE 4096.0f",
        "",
        "struct smart_io_suite_model {",
        "\tconst char *name;",
        "\tconst float *fp_weight[SMART_IO_SUITE_MAX_LAYERS];",
        "\tconst float *fp_bias[SMART_IO_SUITE_MAX_LAYERS];",
        "\tconst s8 *int8_weight[SMART_IO_SUITE_MAX_LAYERS];",
        "\tconst s32 *int8_bias[SMART_IO_SUITE_MAX_LAYERS];",
        "\tconst s32 *int8_multiplier[SMART_IO_SUITE_MAX_LAYERS];",
        "\tconst float *int8_weight_scale[SMART_IO_SUITE_MAX_LAYERS];",
        "\tconst float *activation_scale;",
        "\tu32 layer_count;",
        "\tu32 dims[SMART_IO_SUITE_MAX_DIMS];",
        "};",
        "",
        "static const float smart_io_suite_mean[11] = {",
        c_values(mean, "float"),
        "};",
        "static const float smart_io_suite_std[11] = {",
        c_values(std, "float"),
        "};",
        "static const float smart_io_suite_clip = 5.0f;",
        "",
    ]

    for model_id, (name, dims, is_random) in enumerate(ARCHITECTURES):
        layers = random_weights(dims, 20260811 + model_id) if is_random else actual_weights(args.model_dir / "weights_fp32.npz")
        if tuple(dims) != (INPUTS, *[weight.shape[0] for weight, _ in layers]):
            raise ValueError(f"{name}: architecture mismatch")
        quantized = quantize(layers, normalized)
        record = {
            "id": model_id,
            "name": name,
            "dims": list(dims),
            "parameter_count": int(sum(weight.size + bias.size for weight, bias in layers)),
            "fp32_parameter_bytes": int(sum((weight.size + bias.size) * 4 for weight, bias in layers)),
            "int8_payload_bytes": int(
                sum(weight.size + bias.size * 4 + bias.size * 4 + bias.size * 4
                    for weight, bias in layers)
                + len(quantized["activation_scales"]) * 4
            ),
            "seed": None if not is_random else 20260811 + model_id,
        }
        models.append(record)
        for layer_id, (weight, bias) in enumerate(layers):
            prefix = f"smart_io_suite_m{model_id}_l{layer_id}"
            header.extend([
                f"static const float {prefix}_fp_weight[] = {{",
                c_values(weight, "float"),
                "};",
                f"static const float {prefix}_fp_bias[] = {{",
                c_values(bias, "float"),
                "};",
                f"static const s8 {prefix}_int8_weight[] = {{",
                c_values(quantized["weights"][layer_id], "s8", 16),
                "};",
                f"static const s32 {prefix}_int8_bias[] = {{",
                c_values(quantized["biases"][layer_id], "s32"),
                "};",
                f"static const s32 {prefix}_int8_multiplier[] = {{",
                c_values(quantized["multipliers"][layer_id], "s32"),
                "};",
                f"static const float {prefix}_int8_weight_scale[] = {{",
                c_values(quantized["weight_scales"][layer_id], "float"),
                "};",
            ])
        header.extend([
            f"static const float smart_io_suite_m{model_id}_activation_scale[] = {{",
            c_values(quantized["activation_scales"], "float"),
            "};",
            "",
        ])

    header.append("static const struct smart_io_suite_model smart_io_suite_models[] = {")
    for model_id, (_, dims, _) in enumerate(ARCHITECTURES):
        layer_count = len(dims) - 1
        fp_weight = ", ".join(
            f"smart_io_suite_m{model_id}_l{layer}_fp_weight" if layer < layer_count else "NULL"
            for layer in range(4)
        )
        fp_bias = ", ".join(
            f"smart_io_suite_m{model_id}_l{layer}_fp_bias" if layer < layer_count else "NULL"
            for layer in range(4)
        )
        int8_weight = ", ".join(
            f"smart_io_suite_m{model_id}_l{layer}_int8_weight" if layer < layer_count else "NULL"
            for layer in range(4)
        )
        int8_bias = ", ".join(
            f"smart_io_suite_m{model_id}_l{layer}_int8_bias" if layer < layer_count else "NULL"
            for layer in range(4)
        )
        multiplier = ", ".join(
            f"smart_io_suite_m{model_id}_l{layer}_int8_multiplier" if layer < layer_count else "NULL"
            for layer in range(4)
        )
        weight_scale = ", ".join(
            f"smart_io_suite_m{model_id}_l{layer}_int8_weight_scale" if layer < layer_count else "NULL"
            for layer in range(4)
        )
        padded_dims = list(dims) + [0] * (5 - len(dims))
        header.extend([
            "\t{",
            f"\t\t.name = \"{ARCHITECTURES[model_id][0]}\",",
            f"\t\t.fp_weight = {{ {fp_weight} }},",
            f"\t\t.fp_bias = {{ {fp_bias} }},",
            f"\t\t.int8_weight = {{ {int8_weight} }},",
            f"\t\t.int8_bias = {{ {int8_bias} }},",
            f"\t\t.int8_multiplier = {{ {multiplier} }},",
            f"\t\t.int8_weight_scale = {{ {weight_scale} }},",
            f"\t\t.activation_scale = smart_io_suite_m{model_id}_activation_scale,",
            f"\t\t.layer_count = {layer_count}U,",
            "\t\t.dims = { " + ", ".join(f"{d}U" for d in padded_dims) + " },",
            "\t},",
        ])
    header.extend(["};", "", "#endif", ""])
    args.header.write_text("\n".join(header), encoding="ascii")
    args.summary.write_text(json.dumps({
        "model_count": len(models),
        "input_rows": int(len(states)),
        "input_q_scale": INPUT_Q_SCALE,
        "output_q_scale": OUTPUT_Q_SCALE,
        "requant_multiplier_shift": MULT_SHIFT,
        "models": models,
    }, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
