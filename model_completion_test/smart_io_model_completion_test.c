// SPDX-License-Identifier: GPL-2.0-only
#include <asm/neon.h>
#include <asm/simd.h>

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include <trace/events/block.h>

#include "smart_io_model_fp32.h"
#include "smart_io_model_fp32_params.h"
#include "smart_io_throttle.h"

extern void smart_io_fp32_infer(const struct smart_io_model_fp32_bits *model,
				const struct smart_io_model_fp32_scaler_bits *scaler,
				const u32 input_bits[SMART_IO_MODEL_FP32_INPUTS],
				u32 *action_id,
				u32 q_bits[SMART_IO_MODEL_FP32_ACTIONS]);

static struct tracepoint *smart_io_completion_tp;
static atomic_t smart_io_completion_callbacks = ATOMIC_INIT(0);
static atomic_t smart_io_completion_simd_allowed = ATOMIC_INIT(0);
static atomic_t smart_io_completion_success = ATOMIC_INIT(0);
static atomic_t smart_io_completion_not_allowed = ATOMIC_INIT(0);
static atomic_t smart_io_completion_errors = ATOMIC_INIT(0);
static atomic_t smart_io_completion_log_count = ATOMIC_INIT(0);

static const struct smart_io_state_snapshot smart_io_completion_state = {
	.device_q_count = 8,
	.device_q_fg_count = 2,
	.device_q_slow_count = 4,
	.queue_max = 32,
	.dev_lat_threshold_us = 1500,
	.window_complete_count = SMART_IO_DEV_LAT_WINDOW,
	.window_mean_dev_us = 2100,
	.window_span_us = 5000,
	.window_slow_count = 5,
	.window_fg_complete_count = 1,
	.waiting_total_count = 12,
	.waiting_count = 9,
	.waiting_fg = 2,
	.waiting_high_ioprio_count = 1,
	.waiting_to_issued_contiguous_count = 3,
};

static const u32 smart_io_completion_input_bits[SMART_IO_MODEL_FP32_INPUTS] = {
	0x3e800000, 0x3e800000, 0x3f000000, 0x3fb33333,
	0x3f000000, 0x3f800000, 0x3e4ccccd, 0x3e900000,
	0x3d800000, 0x3daaaaab, 0x3e800000,
};

static int smart_io_completion_raw_infer(u32 *action_id)
{
	u32 q_bits[SMART_IO_MODEL_FP32_ACTIONS];

	if (!may_use_simd())
		return -EBUSY;
	kernel_neon_begin();
	smart_io_fp32_infer(&smart_io_fp32_model, &smart_io_fp32_scaler,
			    smart_io_completion_input_bits, action_id, q_bits);
	kernel_neon_end();
	return *action_id < SMART_IO_MODEL_FP32_ACTIONS ? 0 : -ERANGE;
}

static void smart_io_completion_probe(void *ignore, struct request *rq,
				      blk_status_t status, unsigned int nr_bytes)
{
	u32 action_id = U32_MAX;
	u32 raw_action_id = U32_MAX;
	u64 start_ns;
	u64 full_duration_ns;
	u64 raw_duration_ns;
	u64 neon_pair_ns = 0;
	bool allowed;
	int raw_ret;
	int ret;
	int log_no;
	int cpu;

	(void)ignore;
	(void)rq;
	(void)status;
	(void)nr_bytes;
	cpu = raw_smp_processor_id();
	atomic_inc(&smart_io_completion_callbacks);
	allowed = may_use_simd();
	if (allowed)
		atomic_inc(&smart_io_completion_simd_allowed);

	if (allowed) {
		start_ns = ktime_get_ns();
		kernel_neon_begin();
		kernel_neon_end();
		neon_pair_ns = ktime_get_ns() - start_ns;
	}
	start_ns = ktime_get_ns();
	raw_ret = smart_io_completion_raw_infer(&raw_action_id);
	raw_duration_ns = ktime_get_ns() - start_ns;
	start_ns = ktime_get_ns();
	ret = smart_io_model_fp32_predict(&smart_io_completion_state, &action_id);
	full_duration_ns = ktime_get_ns() - start_ns;
	if (!allowed)
		atomic_inc(&smart_io_completion_not_allowed);
	if (!ret && action_id == 11)
		atomic_inc(&smart_io_completion_success);
	else
		atomic_inc(&smart_io_completion_errors);

	log_no = atomic_inc_return(&smart_io_completion_log_count);
	if (log_no <= 8)
		pr_info("smart_io_model_completion_test: n=%d cpu=%d simd=%u hardirq=%u softirq=%u irqs_disabled=%u pair_ns=%llu raw_ret=%d raw_action=%u raw_ns=%llu full_ret=%d action=%u full_ns=%llu\n",
			log_no, cpu, allowed ? 1U : 0U, in_hardirq() ? 1U : 0U,
			in_serving_softirq() ? 1U : 0U,
			irqs_disabled() ? 1U : 0U, neon_pair_ns, raw_ret,
			raw_action_id, raw_duration_ns, ret, action_id,
			full_duration_ns);
}

static void smart_io_completion_find_tracepoint(struct tracepoint *tp,
						 void *ignore)
{
	(void)ignore;
	if (!strcmp(tp->name, "block_rq_complete"))
		smart_io_completion_tp = tp;
}

static int __init smart_io_model_completion_test_init(void)
{
	u32 action_id = U32_MAX;
	u64 start_ns;
	u64 duration_ns;
	int ret;

	start_ns = ktime_get_ns();
	ret = smart_io_model_fp32_predict(&smart_io_completion_state, &action_id);
	duration_ns = ktime_get_ns() - start_ns;
	pr_info("smart_io_model_completion_test: task_context cpu=%d softirq=%u ret=%d action=%u full_ns=%llu\n",
		raw_smp_processor_id(), in_serving_softirq() ? 1U : 0U,
		ret, action_id, duration_ns);
	if (ret || action_id != 11)
		return ret ? ret : -EINVAL;

	for_each_kernel_tracepoint(smart_io_completion_find_tracepoint, NULL);
	if (!smart_io_completion_tp)
		return -ENOENT;

	ret = tracepoint_probe_register(smart_io_completion_tp,
					  smart_io_completion_probe, NULL);
	if (ret)
		return ret;

	pr_info("smart_io_model_completion_test: block_rq_complete probe registered\n");
	return 0;
}

static void __exit smart_io_model_completion_test_exit(void)
{
	tracepoint_probe_unregister(smart_io_completion_tp,
				   smart_io_completion_probe, NULL);
	tracepoint_synchronize_unregister();
	pr_info("smart_io_model_completion_test: callbacks=%d simd_allowed=%d success=%d not_allowed=%d errors=%d\n",
		atomic_read(&smart_io_completion_callbacks),
		atomic_read(&smart_io_completion_simd_allowed),
		atomic_read(&smart_io_completion_success),
		atomic_read(&smart_io_completion_not_allowed),
		atomic_read(&smart_io_completion_errors));
}

module_init(smart_io_model_completion_test_init);
module_exit(smart_io_model_completion_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Smart I/O FP32 direct completion-context test");
