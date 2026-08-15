/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SMART_IO_MODEL_INT8_H
#define SMART_IO_MODEL_INT8_H

#include "smart_io_model_bench_uapi.h"

#define SMART_IO_MODEL_SEED 0x13579bdfU
#define SMART_IO_MODEL_FNV_OFFSET 2166136261U
#define SMART_IO_MODEL_FNV_PRIME 16777619U

#ifndef SMART_IO_MODEL_NOINLINE
#define SMART_IO_MODEL_NOINLINE noinline
#endif

struct smart_io_model_int8 {
	__s8 weight1[SMART_IO_MODEL_HIDDEN1][SMART_IO_MODEL_INPUTS];
	__s32 bias1[SMART_IO_MODEL_HIDDEN1];
	__s8 weight2[SMART_IO_MODEL_HIDDEN2][SMART_IO_MODEL_HIDDEN1];
	__s32 bias2[SMART_IO_MODEL_HIDDEN2];
	__s8 level_weight[SMART_IO_MODEL_LEVEL_ACTIONS][SMART_IO_MODEL_HIDDEN2];
	__s32 level_bias[SMART_IO_MODEL_LEVEL_ACTIONS];
	__s8 policy_weight[SMART_IO_MODEL_POLICY_ACTIONS][SMART_IO_MODEL_HIDDEN2];
	__s32 policy_bias[SMART_IO_MODEL_POLICY_ACTIONS];
};

static inline __u32 smart_io_model_rng_next(__u32 *state)
{
	*state = *state * 1664525U + 1013904223U;
	return *state;
}

static inline __s8 smart_io_model_next_weight(__u32 *state)
{
	return (__s8)((smart_io_model_rng_next(state) >> 28) - 4);
}

static inline __s32 smart_io_model_next_bias(__u32 *state)
{
	return (__s32)((smart_io_model_rng_next(state) >> 26) - 32);
}

static inline void smart_io_model_int8_init(struct smart_io_model_int8 *model)
{
	__u32 state = SMART_IO_MODEL_SEED;
	int input;
	int output;

	for (output = 0; output < SMART_IO_MODEL_HIDDEN1; output++) {
		for (input = 0; input < SMART_IO_MODEL_INPUTS; input++)
			model->weight1[output][input] =
				smart_io_model_next_weight(&state);
		model->bias1[output] = smart_io_model_next_bias(&state);
	}
	for (output = 0; output < SMART_IO_MODEL_HIDDEN2; output++) {
		for (input = 0; input < SMART_IO_MODEL_HIDDEN1; input++)
			model->weight2[output][input] =
				smart_io_model_next_weight(&state);
		model->bias2[output] = smart_io_model_next_bias(&state);
	}
	for (output = 0; output < SMART_IO_MODEL_LEVEL_ACTIONS; output++) {
		for (input = 0; input < SMART_IO_MODEL_HIDDEN2; input++)
			model->level_weight[output][input] =
				smart_io_model_next_weight(&state);
		model->level_bias[output] = smart_io_model_next_bias(&state);
	}
	for (output = 0; output < SMART_IO_MODEL_POLICY_ACTIONS; output++) {
		for (input = 0; input < SMART_IO_MODEL_HIDDEN2; input++)
			model->policy_weight[output][input] =
				smart_io_model_next_weight(&state);
		model->policy_bias[output] = smart_io_model_next_bias(&state);
	}
}

static inline void smart_io_model_fill_input(__u64 sample_id,
					     __s8 input[SMART_IO_MODEL_INPUTS])
{
	__u32 state = (__u32)sample_id ^ (__u32)(sample_id >> 32) ^
		SMART_IO_MODEL_SEED;
	int i;

	for (i = 0; i < SMART_IO_MODEL_INPUTS; i++)
		input[i] = (__s8)((smart_io_model_rng_next(&state) >> 25) - 64);
}

static inline __s16 smart_io_model_relu_shift(__s32 value, unsigned int shift)
{
	if (value <= 0)
		return 0;
	value >>= shift;
	if (value > 127)
		return 127;
	return (__s16)value;
}

static SMART_IO_MODEL_NOINLINE void
smart_io_model_int8_infer(const struct smart_io_model_int8 *model,
			  const __s8 input[SMART_IO_MODEL_INPUTS],
			  __u32 *level_action, __u32 *policy_action,
			  __u32 *output_checksum)
{
	__s16 hidden1[SMART_IO_MODEL_HIDDEN1];
	__s16 hidden2[SMART_IO_MODEL_HIDDEN2];
	__s32 best_level_score = 0;
	__s32 best_policy_score = 0;
	__u32 best_level = 0;
	__u32 best_policy = 0;
	__u32 checksum = SMART_IO_MODEL_FNV_OFFSET;
	int i;
	int output;

	for (output = 0; output < SMART_IO_MODEL_HIDDEN1; output++) {
		__s32 accumulator = model->bias1[output];

		for (i = 0; i < SMART_IO_MODEL_INPUTS; i++)
			accumulator += input[i] * model->weight1[output][i];
		hidden1[output] = smart_io_model_relu_shift(accumulator, 3);
	}
	for (output = 0; output < SMART_IO_MODEL_HIDDEN2; output++) {
		__s32 accumulator = model->bias2[output];

		for (i = 0; i < SMART_IO_MODEL_HIDDEN1; i++)
			accumulator += hidden1[i] * model->weight2[output][i];
		hidden2[output] = smart_io_model_relu_shift(accumulator, 6);
	}
	for (output = 0; output < SMART_IO_MODEL_LEVEL_ACTIONS; output++) {
		__s32 score = model->level_bias[output];

		for (i = 0; i < SMART_IO_MODEL_HIDDEN2; i++)
			score += hidden2[i] * model->level_weight[output][i];
		if (!output || score > best_level_score) {
			best_level_score = score;
			best_level = output;
		}
		checksum = (checksum ^ (__u32)score) * SMART_IO_MODEL_FNV_PRIME;
	}
	for (output = 0; output < SMART_IO_MODEL_POLICY_ACTIONS; output++) {
		__s32 score = model->policy_bias[output];

		for (i = 0; i < SMART_IO_MODEL_HIDDEN2; i++)
			score += hidden2[i] * model->policy_weight[output][i];
		if (!output || score > best_policy_score) {
			best_policy_score = score;
			best_policy = output;
		}
		checksum = (checksum ^ (__u32)score) * SMART_IO_MODEL_FNV_PRIME;
	}
	*level_action = best_level;
	*policy_action = best_policy;
	*output_checksum = checksum;
}

#endif
