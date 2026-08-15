#!/usr/bin/env python3
"""Generate ARM64 signed INT8 dot-product forward functions."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from generate_model_suite import (
    ARCHITECTURES,
    MULT_SHIFT,
    actual_weights,
    load_inputs,
    quantize,
    random_weights,
)


def emit_values(lines: list[str], label: str, values: np.ndarray, per_line: int = 16) -> None:
    lines.extend(["\t.align 2", f"{label}:"])
    flat = values.reshape(-1)
    for start in range(0, len(flat), per_line):
        text = ", ".join(str(int(value)) for value in flat[start : start + per_line])
        directive = ".byte" if values.dtype == np.int8 else ".word"
        lines.append(f"\t{directive} {text}")


def emit_function(lines: list[str], model_id: int, dims: tuple[int, ...], qparams: dict) -> None:
    prefix = f"m{model_id}"
    source_offset = 0
    destination_offset = 512
    lines.extend(
        [
            "\t.text",
            "\t.align 2",
            f"\t.global smart_io_suite_int8_{prefix}",
            f"\t.type smart_io_suite_int8_{prefix}, %function",
            f"smart_io_suite_int8_{prefix}:",
            "\tsub sp, sp, #1024",
            "\tmovi v0.16b, #0",
            "\tstr q0, [sp]",
            "\tmov w12, #0",
            "\tmov w7, #0",
            f".L{prefix}_copy_input:",
            "\tldrsb w8, [x0, x7]",
            "\tstrb w8, [sp, x7]",
            "\tadd w7, w7, #1",
            "\tcmp w7, #11",
            f"\tb.lo .L{prefix}_copy_input",
        ]
    )
    for layer_id, (input_count, output_count) in enumerate(zip(dims[:-1], dims[1:])):
        padded_count = (input_count + 15) // 16 * 16
        weight_label = f"smart_io_suite_{prefix}_w{layer_id}"
        bias_label = f"smart_io_suite_{prefix}_b{layer_id}"
        mult_label = f"smart_io_suite_{prefix}_q{layer_id}"
        lines.extend(
            [
                f"\tadrp x5, {weight_label}",
                f"\tadd x5, x5, :lo12:{weight_label}",
                f"\tadrp x6, {bias_label}",
                f"\tadd x6, x6, :lo12:{bias_label}",
                f"\tadrp x14, {mult_label}",
                f"\tadd x14, x14, :lo12:{mult_label}",
                "\tmov w7, #0",
                f".L{prefix}_l{layer_id}_output:",
                f"\tadd x10, sp, #{source_offset}",
                "\tmovi v0.4s, #0",
                "\tmov w8, #0",
                f".L{prefix}_l{layer_id}_group:",
                "\tldr q1, [x10], #16",
                "\tldr q2, [x5], #16",
                "\tsdot v0.4s, v1.16b, v2.16b",
                "\tadd w8, w8, #16",
                f"\tcmp w8, #{padded_count}",
                f"\tb.lo .L{prefix}_l{layer_id}_group",
                "\taddv s0, v0.4s",
                "\tumov w8, v0.s[0]",
                "\tldr w9, [x6, x7, lsl #2]",
                "\tadd w8, w8, w9",
                "\tldr w9, [x14, x7, lsl #2]",
                "\tsxtw x16, w8",
                "\tsxtw x17, w9",
                "\tmul x16, x16, x17",
                f"\tmov x17, #{1 << (MULT_SHIFT - 1)}",
                "\tcmp x16, #0",
                f"\tb.ge .L{prefix}_l{layer_id}_requant_positive",
                "\tneg x16, x16",
                "\tadd x16, x16, x17",
                f"\tasr x16, x16, #{MULT_SHIFT}",
                "\tneg x16, x16",
                f"\tb .L{prefix}_l{layer_id}_requant_done",
                f".L{prefix}_l{layer_id}_requant_positive:",
                "\tadd x16, x16, x17",
                f"\tasr x16, x16, #{MULT_SHIFT}",
                f".L{prefix}_l{layer_id}_requant_done:",
                "\tmov w8, w16",
            ]
        )
        if layer_id + 1 == len(dims) - 1:
            lines.extend(
                [
                    "\tstr w8, [x2, x7, lsl #2]",
                    "\tstr w8, [x3, x7, lsl #2]",
                    "\tcbnz w7, ." + prefix + "_compare",
                    "\tmov w13, w8",
                    "\tb ." + prefix + "_next",
                    f".{prefix}_compare:",
                    "\tcmp w8, w13",
                    f"\tb.le .{prefix}_next",
                    "\tmov w13, w8",
                    "\tmov w12, w7",
                    f".{prefix}_next:",
                ]
            )
        else:
            lines.extend(
                [
                    "\tcmp w8, #0",
                    "\tcsel w8, wzr, w8, le",
                    "\tmov w9, #127",
                    "\tcmp w8, w9",
                    "\tcsel w8, w9, w8, gt",
                    f"\tadd x11, sp, #{destination_offset}",
                    "\tstrb w8, [x11, x7]",
                ]
            )
        lines.extend(
            [
                "\tadd w7, w7, #1",
                f"\tcmp w7, #{output_count}",
                f"\tb.lo .L{prefix}_l{layer_id}_output",
            ]
        )
        source_offset, destination_offset = destination_offset, source_offset
    lines.extend(
        [
            "\tstr w12, [x1]",
            "\tadd sp, sp, #1024",
            "\tret",
            f"\t.size smart_io_suite_int8_{prefix}, .-smart_io_suite_int8_{prefix}",
            "",
        ]
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    spec = json.loads((args.model_dir / "model_spec.json").read_text(encoding="utf-8"))
    _, _, _, normalized = load_inputs(args.dataset, spec)
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-only */",
        "/* Generated by generate_suite_int8_asm.py. */",
        f"\t.arch armv8.2-a+dotprod",
        "\t.section .rodata",
    ]
    for model_id, (_, dims, is_random) in enumerate(ARCHITECTURES):
        layers = (
            random_weights(dims, 20260811 + model_id)
            if is_random
            else actual_weights(args.model_dir / "weights_fp32.npz")
        )
        qparams = quantize(layers, normalized)
        for layer_id in range(len(layers)):
            weight = qparams["weights"][layer_id]
            input_count = dims[layer_id]
            padded_count = (input_count + 15) // 16 * 16
            padded_weight = np.zeros((weight.shape[0], padded_count), dtype=np.int8)
            padded_weight[:, :input_count] = weight
            emit_values(lines, f"smart_io_suite_m{model_id}_w{layer_id}", padded_weight)
            emit_values(lines, f"smart_io_suite_m{model_id}_b{layer_id}", qparams["biases"][layer_id], 8)
            emit_values(lines, f"smart_io_suite_m{model_id}_q{layer_id}", qparams["multipliers"][layer_id], 8)
    for model_id, (_, dims, is_random) in enumerate(ARCHITECTURES):
        layers = (
            random_weights(dims, 20260811 + model_id)
            if is_random
            else actual_weights(args.model_dir / "weights_fp32.npz")
        )
        emit_function(lines, model_id, dims, quantize(layers, normalized))
    args.output.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
