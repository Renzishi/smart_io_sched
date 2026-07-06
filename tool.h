#ifndef SMART_IO_HELPERS_H
#define SMART_IO_HELPERS_H

#include <linux/types.h>

struct request;

#define SMART_IO_RQ_SHARD_BITS 3
#define SMART_IO_UID_HASH_BITS 8
#define SMART_IO_INO_HASH_BITS 8

u32 pages_to_mb(unsigned long pages);
u8 encode_io_op(unsigned int op);
bool io_op_is_write(u8 op);
u8 lookup_device_type(const char *name);
u8 match_thread_role(uid_t uid, pid_t pid, pid_t tid, const char *comm, uid_t fg_uid);
u32 kb_window_to_kbps(u64 kb, u32 window_ms);
u32 calc_util_pct(u32 cur_value, u32 max_value);
u8 lat_bucket_idx(u64 latency_us, u8 max_buckets);
u32 lat_bucket_value(u8 idx);
unsigned int rq_shard_idx(const struct request *rq);
unsigned int uid_hash_idx(u32 uid);
unsigned int ino_hash_idx(u64 ino);
u32 get_swap_used_mb(void);
u32 get_runqueue_len(void);
u64 calc_reclaim_pages(void);
bool smart_io_snapshot_task_state(pid_t tid, unsigned long *state_out, int *cpuid_out, u64 *u_s_time_out);
u32 smart_io_task_state_to_enum(unsigned long state);
const char *smart_io_task_state_to_str(u32 state);

static inline u32 fnv1a_32(const char *str)
{
	u32 hash = 0x811c9dc5U;
	const u8 *p = (const u8 *)str;

	while (*p) {
		hash ^= *p++;
		hash *= 0x01000193U;
	}
	return hash;
}

#endif
