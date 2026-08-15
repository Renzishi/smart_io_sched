// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "smart_io_model_fp32.h"
#include "smart_io_throttle.h"

struct smart_io_model_test_case {
	struct smart_io_state_snapshot state;
	u32 expected_action;
};

static const struct smart_io_model_test_case smart_io_model_test_cases[] = {
	{
		.state = {
			.device_q_count = 8, .device_q_fg_count = 2,
			.device_q_slow_count = 4, .queue_max = 32,
			.dev_lat_threshold_us = 1500, .window_complete_count = 5,
			.window_mean_dev_us = 2100, .window_span_us = 5000,
			.window_slow_count = 5, .window_fg_complete_count = 1,
			.waiting_total_count = 12, .waiting_count = 9,
			.waiting_fg = 2, .waiting_high_ioprio_count = 1,
			.waiting_to_issued_contiguous_count = 3,
		},
		.expected_action = 11,
	},
	{
		.state = {
			.queue_max = 16, .dev_lat_threshold_us = 1500,
			.window_complete_count = 5, .window_mean_dev_us = 1500,
			.window_slow_count = 5,
		},
		.expected_action = 7,
	},
	{
		.state = {
			.device_q_count = 64, .device_q_fg_count = 64,
			.device_q_slow_count = 64, .queue_max = 64,
			.dev_lat_threshold_us = 1500, .window_complete_count = 5,
			.window_mean_dev_us = 3000, .window_span_us = 10000,
			.window_slow_count = 5, .window_fg_complete_count = 5,
			.waiting_total_count = 64, .waiting_count = 64,
			.waiting_fg = 64, .waiting_high_ioprio_count = 64,
			.waiting_to_issued_contiguous_count = 64,
		},
		.expected_action = 1,
	},
	{
		.state = {
			.device_q_count = 17, .device_q_fg_count = 3,
			.device_q_slow_count = 11, .queue_max = 128,
			.dev_lat_threshold_us = 1700, .window_complete_count = 5,
			.window_mean_dev_us = 2900, .window_span_us = 9999,
			.window_slow_count = 5, .window_fg_complete_count = 4,
			.waiting_total_count = 31, .waiting_count = 25,
			.waiting_fg = 2, .waiting_high_ioprio_count = 3,
			.waiting_to_issued_contiguous_count = 9,
		},
		.expected_action = 11,
	},
	{
		.state = {
			.device_q_count = 3, .device_q_fg_count = 1,
			.device_q_slow_count = 2, .queue_max = 8,
			.dev_lat_threshold_us = 1000, .window_complete_count = 5,
			.window_mean_dev_us = 7654, .window_span_us = 3333,
			.window_slow_count = 5, .window_fg_complete_count = 3,
			.waiting_total_count = 7, .waiting_count = 5,
			.waiting_fg = 1, .waiting_high_ioprio_count = 2,
			.waiting_to_issued_contiguous_count = 4,
		},
		.expected_action = 2,
	},
};

static int __init smart_io_model_direct_test_init(void)
{
	u32 action_id;
	int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(smart_io_model_test_cases); i++) {
		ret = smart_io_model_fp32_predict(&smart_io_model_test_cases[i].state,
						  &action_id);
		if (ret || action_id != smart_io_model_test_cases[i].expected_action) {
			pr_err("smart_io_model_direct_test: case=%d ret=%d action=%u expected=%u\n",
			       i, ret, action_id,
			       smart_io_model_test_cases[i].expected_action);
			return ret ? ret : -EINVAL;
		}
	}

	pr_info("smart_io_model_direct_test: 5/5 FP32 direct cases passed\n");
	return 0;
}

static void __exit smart_io_model_direct_test_exit(void)
{
}

module_init(smart_io_model_direct_test_init);
module_exit(smart_io_model_direct_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Smart I/O direct FP32 model smoke test");
