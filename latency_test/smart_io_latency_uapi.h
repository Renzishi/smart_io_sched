/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SMART_IO_LATENCY_UAPI_H
#define SMART_IO_LATENCY_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SMART_IO_LATENCY_ABI_VERSION 1U
#define SMART_IO_LATENCY_DEVICE_NAME "smart_io_latency"
#define SMART_IO_LATENCY_DEADLINE_NS 3000000ULL

#define SMART_IO_LATENCY_THROTTLE_MEDIUM 2U
#define SMART_IO_LATENCY_DISPATCH_BASELINE 0U

enum smart_io_latency_status {
	SMART_IO_LATENCY_STATUS_NONE,
	SMART_IO_LATENCY_STATUS_APPLIED,
	SMART_IO_LATENCY_STATUS_LATE,
	SMART_IO_LATENCY_STATUS_NO_WAITER,
};

struct smart_io_latency_request {
	__u32 version;
	__u32 size;
	__u64 sequence_id;
	__u32 trigger_delay_us;
	__u32 reserved;
};

struct smart_io_latency_action {
	__u32 version;
	__u32 size;
	__u64 sequence_id;
	__u32 throttle_level;
	__u32 dispatch_policy;
};

struct smart_io_latency_result {
	__u32 version;
	__u32 size;
	__u64 sequence_id;
	__u64 wake_ts_ns;
	__u64 write_enter_ts_ns;
	__u64 decision_end_ts_ns;
	__u64 wake_to_write_enter_ns;
	__u64 kernel_apply_ns;
	__u64 total_ns;
	__u32 trigger_cpu;
	__u32 write_cpu;
	__u32 waiter_present;
	__u32 status;
};

#define SMART_IO_LATENCY_IOC_MAGIC 0xb7
#define SMART_IO_LATENCY_IOC_GET_LAST \
	_IOR(SMART_IO_LATENCY_IOC_MAGIC, 1, struct smart_io_latency_result)

#endif
