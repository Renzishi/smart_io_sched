#ifndef SMART_IO_TYPES_H
#define SMART_IO_TYPES_H

#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/types.h>

#define SMART_IO_FILE_EXT_STR_LEN 64
#define SMART_IO_FS_TYPE_LEN 16
#define SMART_IO_DEV_NAME_LEN 32

enum smart_io_file_ext {
	SMART_IO_FILE_EXT_UNKNOWN = 0,
	SMART_IO_FILE_EXT_DB = 1,
	SMART_IO_FILE_EXT_SO = 2,
	SMART_IO_FILE_EXT_DEX = 3,
	SMART_IO_FILE_EXT_APK = 4,
	SMART_IO_FILE_EXT_LOG = 5,
	SMART_IO_FILE_EXT_VDEX = 6,
	SMART_IO_FILE_EXT_ODEX = 7,
};

enum smart_io_thread_role {
	ROLE_UNKNOWN = 0,
	ROLE_UX = 1,
	ROLE_FRONT = 2,
	ROLE_BACK = 3,
	ROLE_KWORKER = 4,
	ROLE_FS_BACK = 5,
	ROLE_FIO = 6,
};

enum smart_io_device_type {
	DEV_UNKNOWN = 0,
	DEV_UFS = 1,
	DEV_EMMC = 2,
	DEV_SD = 3,
	DEV_ZRAM = 4,
};

enum smart_io_op_type {
	SMART_IO_OP_READ = 0,
	SMART_IO_OP_WRITE = 1,
	SMART_IO_OP_FSYNC = 2,
	SMART_IO_OP_DISCARD = 3,
};

enum smart_io_task_state {
	SMART_IO_TASK_UNKNOWN = 0,
	SMART_IO_TASK_RUNNING = 1,
	SMART_IO_TASK_RUNNABLE = 2,
	SMART_IO_TASK_INTERRUPTIBLE = 3,
	SMART_IO_TASK_UNINTERRUPTIBLE = 4,
	SMART_IO_TASK_STOPPED = 5,
	SMART_IO_TASK_TRACED = 6,
	SMART_IO_TASK_DEAD = 7,
	SMART_IO_TASK_ZOMBIE = 8,
	SMART_IO_TASK_PARKED = 9,
	SMART_IO_TASK_IDLE = 10,
};

struct smart_io_event {
	u64 ts_insert;
	u64 ts_issue;
	u64 ts_requeue;
	u64 ts_complete;

	u32 uid;
	u32 scene_tag;
	u8 is_front;
	u8 thread_role;

	u8 io_op;
	u32 io_size_kb;
	u64 sector;
	u16 ioprio_class;
	u16 file_ext;
	char file_ext_str[SMART_IO_FILE_EXT_STR_LEN];
	char fs_type[SMART_IO_FS_TYPE_LEN];
	char dev_name[SMART_IO_DEV_NAME_LEN];
	char remap_from[SMART_IO_DEV_NAME_LEN];
	u8 device_type;
	u8 remap_mixed;
	u8 is_sync;
	u64 inode_hash;
	u64 folio_index;

	u32 io_in_flight;
	u32 queue_saturate;
	u32 cgroup_io_weight;

	u8 f2fs_gc_active;
	u8 f2fs_cp_active;
	u8 is_hot_file;

	u32 mem_avail_mb;
	u32 dirty_pages_mb;
	u32 wb_pages_mb;
	u32 swap_mb;
	u32 cpu_loadavg_1m;
	u8 online_cpus;
	u32 runqueue_len;
	u8 device_util_pct;
	u8 insert_cpuid;
	u8 issue_cpuid;

	u32 io_bw_kbps;

	u64 latency_us;
	u64 sched_us;
	u64 dev_us;
	u32 sched_pct;

	u32 p50_read_lat_us;
	u32 p50_write_lat_us;
	u32 p99_read_lat_us;
	u32 p99_write_lat_us;
	u32 avg_read_lat_us;
	u32 avg_write_lat_us;
	u32 p50_p99_ratio_r;
	u32 p50_p99_ratio_w;
	u64 reclaim_pages;
	u32 sys_psi_io;
	u32 blk_bw_util;

	u32 render_avg_lat;
	u32 jank_cnt;
	u32 time_to_first_frame;
	u32 time_to_input_response;
	u32 cache_hit;
	u64 file_access_pages;
	u64 file_miss_pages;
	u64 file_cache_add_pages;
	u64 file_cache_delete_pages;
	u64 file_refault_pages;
	u64 file_fault_count;
	u64 file_fault_wait_us;

	u8 direct_path;
};

struct smart_io_counters {
	u64 insert_cnt;
	u64 issue_cnt;
	u64 complete_cnt;
	u64 direct_issue_cnt;
	u64 lost_complete_cnt;
};

DECLARE_PER_CPU(struct smart_io_counters, smart_io_cnts);
extern atomic_t smart_io_enabled;
extern atomic_t rawdata_trace_enabled;
extern atomic_t debug_enabled;

#endif
