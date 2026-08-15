// SPDX-License-Identifier: GPL-2.0-only
#include <asm/neon.h>
#include <asm/simd.h>

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>

#include "smart_io_model_fp32.h"
#include "smart_io_model_fp32_params.h"
#include "smart_io_throttle.h"

extern void smart_io_fp32_infer(const struct smart_io_model_fp32_bits *model,
				const struct smart_io_model_fp32_scaler_bits *scaler,
				const u32 input_bits[SMART_IO_MODEL_FP32_INPUTS],
				u32 *action_id,
				u32 q_bits[SMART_IO_MODEL_FP32_ACTIONS]);

static_assert(sizeof(struct smart_io_model_fp32_scaler_bits) == 92);
static_assert(sizeof(struct smart_io_model_fp32_bits) == 7344);

static bool smart_io_model_ratio_bits(u64 numerator, u64 denominator,
				      u32 *bits)
{
	u64 scaled;
	u64 quotient;
	u64 remainder;
	int exponent;
	int shift;

	if (!denominator || !bits)
		return false;
	if (!numerator) {
		*bits = 0;
		return true;
	}

	exponent = fls64(numerator) - fls64(denominator);
	if (exponent >= 0) {
		if (denominator > U64_MAX >> exponent ||
		    numerator < (denominator << exponent))
			exponent--;
	} else if (numerator > U64_MAX >> -exponent ||
		   (numerator << -exponent) < denominator) {
		exponent--;
	}

	shift = 23 - exponent;
	if (shift < 0 || shift >= 64 || numerator > U64_MAX >> shift)
		return false;
	scaled = numerator << shift;
	quotient = div64_u64_rem(scaled, denominator, &remainder);
	if (remainder > denominator - remainder ||
	    (remainder == denominator - remainder && (quotient & 1)))
		quotient++;
	if (quotient == BIT_ULL(24)) {
		quotient >>= 1;
		exponent++;
	}
	if (quotient < BIT_ULL(23) || quotient >= BIT_ULL(24) ||
	    exponent < -126 || exponent > 127)
		return false;

	*bits = ((u32)(exponent + 127) << 23) |
		((u32)quotient - BIT(23));
	return true;
}

static bool smart_io_model_q_bits_finite(u32 bits)
{
	return (bits & 0x7f800000U) != 0x7f800000U;
}

int smart_io_model_fp32_predict(const struct smart_io_state_snapshot *state,
				u32 *action_id)
{
	u32 input_bits[SMART_IO_MODEL_FP32_INPUTS];
	u32 q_bits[SMART_IO_MODEL_FP32_ACTIONS];
	u32 waiting_count;
	u32 waiting_fg;
	int i;

	if (!state || !action_id || !state->queue_max ||
	    !state->dev_lat_threshold_us ||
	    state->window_complete_count != SMART_IO_DEV_LAT_WINDOW ||
	    state->window_span_us > SMART_IO_DEV_LAT_WINDOW_MAX_US ||
	    state->device_q_count > state->queue_max ||
	    state->device_q_fg_count > state->device_q_count ||
	    state->device_q_slow_count > state->device_q_count ||
	    state->window_slow_count > state->window_complete_count ||
	    state->window_fg_complete_count > state->window_complete_count ||
	    state->waiting_count > state->waiting_total_count ||
	    state->waiting_fg > state->waiting_total_count ||
	    state->waiting_high_ioprio_count > state->waiting_total_count ||
	    state->waiting_to_issued_contiguous_count > state->waiting_total_count)
		return -EINVAL;

	waiting_count = min(state->waiting_count, state->queue_max);
	waiting_fg = min(state->waiting_fg, state->queue_max);
	if (!smart_io_model_ratio_bits(state->device_q_count, state->queue_max,
				       &input_bits[0]) ||
	    !smart_io_model_ratio_bits(state->device_q_fg_count,
				       max_t(u32, 1, state->device_q_count), &input_bits[1]) ||
	    !smart_io_model_ratio_bits(state->device_q_slow_count,
				       max_t(u32, 1, state->device_q_count), &input_bits[2]) ||
	    !smart_io_model_ratio_bits(state->window_mean_dev_us,
				       state->dev_lat_threshold_us, &input_bits[3]) ||
	    !smart_io_model_ratio_bits(state->window_span_us,
				       SMART_IO_DEV_LAT_WINDOW_MAX_US, &input_bits[4]) ||
	    !smart_io_model_ratio_bits(state->window_slow_count,
				       state->window_complete_count, &input_bits[5]) ||
	    !smart_io_model_ratio_bits(state->window_fg_complete_count,
				       state->window_complete_count, &input_bits[6]) ||
	    !smart_io_model_ratio_bits(waiting_count, state->queue_max,
				       &input_bits[7]) ||
	    !smart_io_model_ratio_bits(waiting_fg, state->queue_max,
				       &input_bits[8]) ||
	    !smart_io_model_ratio_bits(state->waiting_high_ioprio_count,
				       max_t(u32, 1, state->waiting_total_count), &input_bits[9]) ||
	    !smart_io_model_ratio_bits(state->waiting_to_issued_contiguous_count,
				       max_t(u32, 1, state->waiting_total_count), &input_bits[10]))
		return -ERANGE;

	if (!may_use_simd())
		return -EBUSY;
	kernel_neon_begin();
	smart_io_fp32_infer(&smart_io_fp32_model, &smart_io_fp32_scaler,
			    input_bits, action_id, q_bits);
	kernel_neon_end();
	if (*action_id >= SMART_IO_MODEL_FP32_ACTIONS)
		return -ERANGE;
	for (i = 0; i < SMART_IO_MODEL_FP32_ACTIONS; i++) {
		if (!smart_io_model_q_bits_finite(q_bits[i]))
			return -ERANGE;
	}

	return 0;
}
