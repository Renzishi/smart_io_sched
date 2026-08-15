/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SMART_IO_MODEL_FP32_NEON_H
#define SMART_IO_MODEL_FP32_NEON_H

#include "smart_io_model_bench_uapi.h"

/* Keep the benchmark ABI names aligned with the production FP32 model. */
#define SMART_IO_MODEL_FP32_INPUTS SMART_IO_MODEL_INPUTS
#define SMART_IO_MODEL_FP32_HIDDEN 32U
#define SMART_IO_MODEL_FP32_ACTIONS SMART_IO_MODEL_ACTIONS

#define SMART_IO_FP32_BIAS1_OFFSET 1408
#define SMART_IO_FP32_WEIGHT2_OFFSET 1536
#define SMART_IO_FP32_BIAS2_OFFSET 5632
#define SMART_IO_FP32_JOINT_WEIGHT_OFFSET 5760
#define SMART_IO_FP32_JOINT_BIAS_OFFSET 7296
#define SMART_IO_FP32_MODEL_SIZE 7344

#define SMART_IO_FP32_SCALER_STD_OFFSET 44
#define SMART_IO_FP32_SCALER_CLIP_OFFSET 88
#define SMART_IO_FP32_SCALER_SIZE 92

struct smart_io_model_fp32_scaler_bits {
	__u32 mean[SMART_IO_MODEL_INPUTS];
	__u32 std[SMART_IO_MODEL_INPUTS];
	__u32 clip;
};

struct smart_io_model_fp32_bits {
	__u32 weight1[SMART_IO_MODEL_HIDDEN1][SMART_IO_MODEL_INPUTS];
	__u32 bias1[SMART_IO_MODEL_HIDDEN1];
	__u32 weight2[SMART_IO_MODEL_HIDDEN2][SMART_IO_MODEL_HIDDEN1];
	__u32 bias2[SMART_IO_MODEL_HIDDEN2];
	__u32 joint_weight[SMART_IO_MODEL_ACTIONS][SMART_IO_MODEL_HIDDEN2];
	__u32 joint_bias[SMART_IO_MODEL_ACTIONS];
};

void smart_io_fp32_infer(const struct smart_io_model_fp32_bits *model,
			 const struct smart_io_model_fp32_scaler_bits *scaler,
			 const __u32 input_bits[SMART_IO_MODEL_INPUTS],
			 __u32 *action_id,
			 __u32 q_bits[SMART_IO_MODEL_Q_VALUES]);

#endif
