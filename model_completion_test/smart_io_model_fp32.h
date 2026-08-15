/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SMART_IO_MODEL_FP32_H
#define SMART_IO_MODEL_FP32_H

#include <linux/types.h>

#define SMART_IO_MODEL_FP32_INPUTS 11U
#define SMART_IO_MODEL_FP32_HIDDEN 32U
#define SMART_IO_MODEL_FP32_ACTIONS 12U

struct smart_io_state_snapshot;

/* Keep these layouts synchronized with smart_io_model_fp32.S offsets. */
struct smart_io_model_fp32_scaler_bits {
	u32 mean[SMART_IO_MODEL_FP32_INPUTS];
	u32 std[SMART_IO_MODEL_FP32_INPUTS];
	u32 clip;
};

struct smart_io_model_fp32_bits {
	u32 weight1[SMART_IO_MODEL_FP32_HIDDEN][SMART_IO_MODEL_FP32_INPUTS];
	u32 bias1[SMART_IO_MODEL_FP32_HIDDEN];
	u32 weight2[SMART_IO_MODEL_FP32_HIDDEN][SMART_IO_MODEL_FP32_HIDDEN];
	u32 bias2[SMART_IO_MODEL_FP32_HIDDEN];
	u32 joint_weight[SMART_IO_MODEL_FP32_ACTIONS][SMART_IO_MODEL_FP32_HIDDEN];
	u32 joint_bias[SMART_IO_MODEL_FP32_ACTIONS];
};

int smart_io_model_fp32_predict(const struct smart_io_state_snapshot *state,
				u32 *action_id);

#endif
