#!/usr/bin/env python3
"""Generate ARM64 FP32 forward functions for the model suite."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from generate_model_suite import ARCHITECTURES, actual_weights, random_weights


def emit_values(lines: list[str], label: str, values: np.ndarray) -> None:
    lines.extend(["\t.align 2", f"{label}:"])
    flat = values.reshape(-1)
    for start in range(0, len(flat), 8):
        values_text = ", ".join(f"{float(value):.9g}" for value in flat[start : start + 8])
        lines.append(f"\t.float {values_text}")


def emit_function(lines: list[str], model_id: int, dims: tuple[int, ...], layers) -> None:
    prefix = f"m{model_id}"
    lines.extend(
        [
            "\t.text",
            "\t.align 2",
            f"\t.global smart_io_suite_fp32_{prefix}",
            f"\t.type smart_io_suite_fp32_{prefix}, %function",
            f"smart_io_suite_fp32_{prefix}:",
            "\tsub sp, sp, #2048",
            "\tmovi v3.2s, #0",
            "\tadrp x5, smart_io_suite_mean",
            "\tadd x5, x5, :lo12:smart_io_suite_mean",
            "\tadrp x6, smart_io_suite_std",
            "\tadd x6, x6, :lo12:smart_io_suite_std",
            "\tadrp x11, smart_io_suite_clip",
            "\tadd x11, x11, :lo12:smart_io_suite_clip",
            "\tldr s4, [x11]",
            "\tfneg s5, s4",
            "\tmov w12, #0",
            "\tmov x9, sp",
            "\tmov w7, #0",
            f".L{prefix}_normalize:",
            "\tldr s0, [x0, x7, lsl #2]",
            "\tldr s1, [x5, x7, lsl #2]",
            "\tldr s2, [x6, x7, lsl #2]",
            "\tfsub s0, s0, s1",
            "\tfdiv s0, s0, s2",
            "\tfmaxnm s0, s0, s5",
            "\tfminnm s0, s0, s4",
            "\tstr s0, [x9, x7, lsl #2]",
            "\tadd w7, w7, #1",
            "\tcmp w7, #11",
            f"\tb.lo .L{prefix}_normalize",
        ]
    )
    source_offset = 0
    destination_offset = 512
    for layer_id, ((weight, bias), (input_count, output_count)) in enumerate(
        zip(layers, zip(dims[:-1], dims[1:]))
    ):
        weight_label = f"smart_io_suite_{prefix}_w{layer_id}"
        bias_label = f"smart_io_suite_{prefix}_b{layer_id}"
        lines.extend(
            [
                f"\tadrp x5, {weight_label}",
                f"\tadd x5, x5, :lo12:{weight_label}",
                f"\tadrp x6, {bias_label}",
                f"\tadd x6, x6, :lo12:{bias_label}",
                "\tmov w7, #0",
                f".L{prefix}_l{layer_id}_output:",
                "\tldr s0, [x6, x7, lsl #2]",
                f"\tadd x10, sp, #{source_offset}",
                "\tmov w8, #0",
                f".L{prefix}_l{layer_id}_input:",
                "\tldr s1, [x10, x8, lsl #2]",
                "\tldr s2, [x5], #4",
                "\tfmadd s0, s1, s2, s0",
                "\tadd w8, w8, #1",
                f"\tcmp w8, #{input_count}",
                f"\tb.lo .L{prefix}_l{layer_id}_input",
            ]
        )
        if layer_id + 1 == len(layers):
            lines.extend(
                [
                    "\tstr s0, [x2, x7, lsl #2]",
                    "\tcbnz w7, .L" + prefix + "_joint_compare",
                    "\tfmov s6, s0",
                    "\tb .L" + prefix + "_joint_next",
                    f".L{prefix}_joint_compare:",
                    "\tfcmp s0, s6",
                    f"\tb.le .L{prefix}_joint_next",
                    "\tfmov s6, s0",
                    "\tmov w12, w7",
                    f".L{prefix}_joint_next:",
                ]
            )
        else:
            lines.extend(
                [
                    "\tfmaxnm s0, s0, s3",
                    f"\tadd x9, sp, #{destination_offset}",
                    "\tstr s0, [x9, x7, lsl #2]",
                ]
            )
        lines.extend(
            [
                "\tadd w7, w7, #1",
                f"\tcmp w7, #{output_count}",
                f"\tb.lo .L{prefix}_l{layer_id}_output",
            ]
        )
        if layer_id + 1 != len(layers):
            source_offset, destination_offset = destination_offset, source_offset
    lines.extend(
        [
            "\tstr w12, [x1]",
            "\tadd sp, sp, #2048",
            "\tret",
            f"\t.size smart_io_suite_fp32_{prefix}, .-smart_io_suite_fp32_{prefix}",
            "",
        ]
    )


def emit_int8_prepare(lines: list[str]) -> None:
    lines.extend(
        [
            "\t.text",
            "\t.align 2",
            "\t.global smart_io_suite_prepare_int8",
            "\t.type smart_io_suite_prepare_int8, %function",
            "smart_io_suite_prepare_int8:",
            "\tadrp x5, smart_io_suite_mean",
            "\tadd x5, x5, :lo12:smart_io_suite_mean",
            "\tadrp x6, smart_io_suite_std",
            "\tadd x6, x6, :lo12:smart_io_suite_std",
            "\tadrp x11, smart_io_suite_clip",
            "\tadd x11, x11, :lo12:smart_io_suite_clip",
            "\tldr s4, [x11]",
            "\tfmov s5, #20.0",
            "\tneg w12, wzr",
            "\tmov w7, #0",
            ".Lprepare_int8_loop:",
            "\tldr s0, [x0, x7, lsl #2]",
            "\tldr s1, [x5, x7, lsl #2]",
            "\tldr s2, [x6, x7, lsl #2]",
            "\tfsub s0, s0, s1",
            "\tfdiv s0, s0, s2",
            "\tfneg s6, s4",
            "\tfmaxnm s0, s0, s6",
            "\tfminnm s0, s0, s4",
            "\tfmul s0, s0, s5",
            "\tfcvtas w8, s0",
            "\tmov w9, #127",
            "\tcmp w8, w9",
            "\tcsel w8, w9, w8, gt",
            "\tmov w9, #-127",
            "\tcmp w8, w9",
            "\tcsel w8, w9, w8, lt",
            "\tstrb w8, [x1, x7]",
            "\tadd w7, w7, #1",
            "\tcmp w7, #11",
            "\tb.lo .Lprepare_int8_loop",
            "\tret",
            "\t.size smart_io_suite_prepare_int8, .-smart_io_suite_prepare_int8",
            "",
        ]
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    spec = json.loads((args.model_dir / "model_spec.json").read_text(encoding="utf-8"))
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-only */",
        "/* Generated by generate_suite_fp32_asm.py. */",
        "\t.section .rodata",
    ]
    mean = np.asarray(spec["preprocessing"]["mean"], dtype=np.float32)
    std = np.asarray(spec["preprocessing"]["std"], dtype=np.float32)
    emit_values(lines, "smart_io_suite_mean", mean)
    emit_values(lines, "smart_io_suite_std", std)
    emit_values(lines, "smart_io_suite_clip", np.asarray([5.0], dtype=np.float32))
    for model_id, (_, dims, is_random) in enumerate(ARCHITECTURES):
        layers = (
            random_weights(dims, 20260811 + model_id)
            if is_random
            else actual_weights(args.model_dir / "weights_fp32.npz")
        )
        for layer_id, (weight, bias) in enumerate(layers):
            emit_values(lines, f"smart_io_suite_m{model_id}_w{layer_id}", weight)
            emit_values(lines, f"smart_io_suite_m{model_id}_b{layer_id}", bias)
    for model_id, (_, dims, is_random) in enumerate(ARCHITECTURES):
        layers = (
            random_weights(dims, 20260811 + model_id)
            if is_random
            else actual_weights(args.model_dir / "weights_fp32.npz")
        )
        emit_function(lines, model_id, dims, layers)
    emit_int8_prepare(lines)
    args.output.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
