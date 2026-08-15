// SPDX-License-Identifier: GPL-2.0-only
#include <asm/neon.h>
#include <asm/simd.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "smart_io_model_bench_uapi.h"
#include "smart_io_model_fp32_neon.h"
#include "smart_io_model_fp32_params.h"
#include "smart_io_model_suite_params.h"

#define SMART_IO_SUITE_MAX_WIDTH 128U

static_assert(SMART_IO_SUITE_MODEL_COUNT == 8U);
static_assert(SMART_IO_SUITE_MAX_DIMS == 5U);

extern void smart_io_suite_prepare_int8(const u32 *, s8 *);
extern void smart_io_suite_int8_m0(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m1(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m2(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m3(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m4(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m5(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m6(const s8 *, u32 *, s32 *, u32 *);
extern void smart_io_suite_int8_m7(const s8 *, u32 *, s32 *, u32 *);

/* The FP32 benchmark must call the exact production implementation. */
extern void smart_io_fp32_infer(
	const struct smart_io_model_fp32_bits *model,
	const struct smart_io_model_fp32_scaler_bits *scaler,
	const u32 input_bits[SMART_IO_MODEL_FP32_INPUTS],
	u32 *action_id, u32 q_bits[SMART_IO_MODEL_FP32_ACTIONS]);

static u32 smart_io_q_checksum(const u32 q_bits[SMART_IO_MODEL_Q_VALUES])
{
	u32 checksum = 2166136261U;
	int i;

	for (i = 0; i < SMART_IO_MODEL_Q_VALUES; i++)
		checksum = (checksum ^ q_bits[i]) * 16777619U;
	return checksum;
}

static long smart_io_model_bench_ioctl(struct file *file, unsigned int command,
				       unsigned long argument)
{
	struct smart_io_model_bench_sample sample;
	u64 start_ns;
	u64 end_ns;

	(void)file;
	if (copy_from_user(&sample, (void __user *)argument, sizeof(sample)))
		return -EFAULT;
	if (sample.version != SMART_IO_MODEL_BENCH_ABI_VERSION ||
	    sample.size != sizeof(sample) ||
	    sample.model_id >= SMART_IO_SUITE_MODEL_COUNT)
		return -EINVAL;
	if (command != SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_FP32 &&
	    command != SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_INT8)
		return -ENOTTY;
	if (!may_use_simd())
		return -EBUSY;

	start_ns = ktime_get_ns();
	kernel_neon_begin();
	if (command == SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_FP32) {
		smart_io_fp32_infer(&smart_io_fp32_model, &smart_io_fp32_scaler,
				    sample.fp32_input_bits, &sample.action_id,
				    sample.fp32_q_bits);
		memset(sample.int8_q, 0, sizeof(sample.int8_q));
	} else {
		smart_io_suite_prepare_int8(sample.fp32_input_bits,
					    sample.int8_input);
		switch (sample.model_id) {
		case 0:
			smart_io_suite_int8_m0(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 1:
			smart_io_suite_int8_m1(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 2:
			smart_io_suite_int8_m2(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 3:
			smart_io_suite_int8_m3(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 4:
			smart_io_suite_int8_m4(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 5:
			smart_io_suite_int8_m5(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 6:
			smart_io_suite_int8_m6(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		case 7:
			smart_io_suite_int8_m7(sample.int8_input, &sample.action_id,
					       sample.int8_q, sample.fp32_q_bits);
			break;
		}
	}
	sample.level_action = sample.action_id / SMART_IO_MODEL_POLICY_ACTIONS;
	sample.policy_action = sample.action_id % SMART_IO_MODEL_POLICY_ACTIONS;
	kernel_neon_end();
	end_ns = ktime_get_ns();

	sample.duration_ns = end_ns - start_ns;
	sample.output_checksum = smart_io_q_checksum(sample.fp32_q_bits);
	if (copy_to_user((void __user *)argument, &sample, sizeof(sample)))
		return -EFAULT;
	return 0;
}

static int smart_io_model_bench_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static const struct file_operations smart_io_model_bench_fops = {
	.owner = THIS_MODULE,
	.open = smart_io_model_bench_open,
	.unlocked_ioctl = smart_io_model_bench_ioctl,
};

static struct miscdevice smart_io_model_bench_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = SMART_IO_MODEL_BENCH_DEVICE_NAME,
	.fops = &smart_io_model_bench_fops,
};

static int __init smart_io_model_bench_init(void)
{
	return misc_register(&smart_io_model_bench_device);
}

static void __exit smart_io_model_bench_exit(void)
{
	misc_deregister(&smart_io_model_bench_device);
}

module_init(smart_io_model_bench_init);
module_exit(smart_io_model_bench_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Smart I/O FP32 and INT8 model suite inference benchmark");
