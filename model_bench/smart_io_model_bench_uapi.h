/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SMART_IO_MODEL_BENCH_UAPI_H
#define SMART_IO_MODEL_BENCH_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SMART_IO_MODEL_BENCH_ABI_VERSION 6U
#define SMART_IO_MODEL_BENCH_DEVICE_NAME "smart_io_model_bench"

#define SMART_IO_MODEL_INPUTS 11
#define SMART_IO_MODEL_HIDDEN1 32
#define SMART_IO_MODEL_HIDDEN2 32
#define SMART_IO_MODEL_LEVEL_ACTIONS 4
#define SMART_IO_MODEL_POLICY_ACTIONS 3
#define SMART_IO_MODEL_ACTIONS \
	(SMART_IO_MODEL_LEVEL_ACTIONS * SMART_IO_MODEL_POLICY_ACTIONS)
#define SMART_IO_MODEL_Q_VALUES SMART_IO_MODEL_ACTIONS

struct smart_io_model_bench_sample {
	__u32 version;
	__u32 size;
	__u32 model_id;
	__u32 reserved;
	__s8 int8_input[SMART_IO_MODEL_INPUTS];
	__u8 reserved_input_bytes;
	__u32 fp32_input_bits[SMART_IO_MODEL_INPUTS];
	__u32 reserved_input;
	__u64 duration_ns;
	__u32 action_id;
	__u32 level_action;
	__u32 policy_action;
	__u32 output_checksum;
	__u32 fp32_q_bits[SMART_IO_MODEL_Q_VALUES];
	__s32 int8_q[SMART_IO_MODEL_Q_VALUES];
};

#define SMART_IO_MODEL_BENCH_IOC_MAGIC 0xb8
#define SMART_IO_MODEL_BENCH_IOC_RUN_EMPTY \
	_IOWR(SMART_IO_MODEL_BENCH_IOC_MAGIC, 2, \
	      struct smart_io_model_bench_sample)
#define SMART_IO_MODEL_BENCH_IOC_RUN_NEON_CONTEXT \
	_IOWR(SMART_IO_MODEL_BENCH_IOC_MAGIC, 3, \
	      struct smart_io_model_bench_sample)
#define SMART_IO_MODEL_BENCH_IOC_RUN_FP32 \
	_IOWR(SMART_IO_MODEL_BENCH_IOC_MAGIC, 4, \
	      struct smart_io_model_bench_sample)
#define SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_FP32 \
	_IOWR(SMART_IO_MODEL_BENCH_IOC_MAGIC, 5, \
	      struct smart_io_model_bench_sample)
#define SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_INT8 \
	_IOWR(SMART_IO_MODEL_BENCH_IOC_MAGIC, 6, \
	      struct smart_io_model_bench_sample)

#endif
