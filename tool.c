#include <linux/blk-mq.h>
#include <linux/hash.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/user_namespace.h>
#include <linux/vmstat.h>

#include <kernel/sched/sched.h>

#include "smart_io_types.h"
#include "tool.h"

u32 pages_to_mb(unsigned long pages)
{
	return (u32)div_u64((u64)pages << PAGE_SHIFT, SZ_1M);
}

u32 pages_to_kb(unsigned long pages)
{
	return (u32)div_u64((u64)pages << PAGE_SHIFT, SZ_1K);
}

u8 encode_io_op(unsigned int op)
{
	switch (op) {
	case REQ_OP_FLUSH:
		return SMART_IO_OP_FSYNC;
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		return SMART_IO_OP_DISCARD;
	case REQ_OP_WRITE:
	case REQ_OP_WRITE_ZEROES:
#ifdef REQ_OP_ZONE_APPEND
	case REQ_OP_ZONE_APPEND:
#endif
		return SMART_IO_OP_WRITE;
	case REQ_OP_READ:
	default:
		return SMART_IO_OP_READ;
	}
}

bool io_op_is_write(u8 op)
{
	return op == SMART_IO_OP_WRITE || op == SMART_IO_OP_FSYNC ||
	       op == SMART_IO_OP_DISCARD;
}

u8 lookup_device_type(const char *name)
{
	if (!name)
		return DEV_UNKNOWN;
	if (strstarts(name, "zram"))
		return DEV_ZRAM;
	if (strstarts(name, "mmcblk"))
		return DEV_EMMC;
	if (strstarts(name, "sd") || strstarts(name, "ufs"))
		return DEV_UFS;
	return DEV_UNKNOWN;
}

u8 match_thread_role(uid_t uid, pid_t pid, pid_t tid, const char *comm, uid_t fg_uid)
{
	if (uid == 0) {
		if (strstarts(comm, "kworker"))
			return ROLE_KWORKER;
		if (strstarts(comm, "jbd2/") || strstarts(comm, "f2fs_"))
			return ROLE_FS_BACK;
		if (strstarts(comm, "fio"))
			return ROLE_FIO;
		return ROLE_UNKNOWN;
	}

	if (uid >= 10000) {
		if (strstr(comm, "RenderThread") || strstr(comm, "GLThread") ||
		    !strcmp(comm, "main"))
			return ROLE_UX;
		if (uid == fg_uid)
			return ROLE_FRONT;
		return ROLE_BACK;
	}

	return ROLE_UNKNOWN;
}

u32 kb_window_to_kbps(u64 kb, u32 window_ms)
{
	u64 kbps;

	if (!window_ms)
		return 0;
	kbps = div64_u64(kb * 1000U, window_ms);
	return (u32)min_t(u64, kbps, U32_MAX);
}

u32 calc_util_pct(u32 cur_value, u32 max_value)
{
	u64 util;

	if (!max_value)
		return 0;
	util = div64_u64((u64)cur_value * 100, max_value);
	return (u32)min_t(u64, util, 100);
}

u8 lat_bucket_idx(u64 latency_us, u8 max_buckets)
{
	u8 idx = 0;

	if (!max_buckets)
		return 0;

	while (latency_us > 1 && idx < max_buckets - 1) {
		latency_us >>= 1;
		idx++;
	}
	return idx;
}

u32 lat_bucket_value(u8 idx)
{
	return 1U << idx;
}

unsigned int rq_shard_idx(const struct request *rq)
{
	return hash_ptr(rq, SMART_IO_RQ_SHARD_BITS);
}

unsigned int uid_hash_idx(u32 uid)
{
	return hash_32(uid, SMART_IO_UID_HASH_BITS);
}

unsigned int ino_hash_idx(u64 ino)
{
	return hash_64(ino, SMART_IO_INO_HASH_BITS);
}

u32 get_swap_used_mb(void)
{
	struct sysinfo info;

	memset(&info, 0, sizeof(info));
	si_swapinfo(&info);
	if (info.totalswap <= info.freeswap)
		return 0;

	return pages_to_mb(info.totalswap - info.freeswap);
}

u32 get_runqueue_len(void)
{
	unsigned int cpu;
	u32 sum = 0;

	for_each_online_cpu(cpu)
		sum += cpu_rq(cpu)->nr_running;

	return sum;
}

u64 calc_reclaim_pages(void)
{
	unsigned long vm_events[NR_VM_EVENT_ITEMS] = { 0 };

	all_vm_events(vm_events);
	return (u64)vm_events[PGSTEAL_KSWAPD] + vm_events[PGSTEAL_DIRECT];
}

u32 smart_io_task_state_to_enum(unsigned long state)
{
	if (state & EXIT_DEAD)
		return SMART_IO_TASK_DEAD;
	if (state & EXIT_ZOMBIE)
		return SMART_IO_TASK_ZOMBIE;
	if (state & TASK_PARKED)
		return SMART_IO_TASK_PARKED;
	if (state & TASK_DEAD)
		return SMART_IO_TASK_DEAD;
	if (state & TASK_TRACED)
		return SMART_IO_TASK_TRACED;
	if (state & TASK_STOPPED)
		return SMART_IO_TASK_STOPPED;
	if (state & TASK_UNINTERRUPTIBLE)
		return SMART_IO_TASK_UNINTERRUPTIBLE;
	if (state & TASK_INTERRUPTIBLE)
		return SMART_IO_TASK_INTERRUPTIBLE;
#ifdef TASK_IDLE
	if (state & TASK_IDLE)
		return SMART_IO_TASK_IDLE;
#endif
	if (state == TASK_RUNNING)
		return SMART_IO_TASK_RUNNING;
	return SMART_IO_TASK_RUNNABLE;
}

const char *smart_io_task_state_to_str(u32 state)
{
	switch (state) {
	case SMART_IO_TASK_RUNNING:
		return "running";
	case SMART_IO_TASK_RUNNABLE:
		return "runnable";
	case SMART_IO_TASK_INTERRUPTIBLE:
		return "interruptible";
	case SMART_IO_TASK_UNINTERRUPTIBLE:
		return "uninterruptible";
	case SMART_IO_TASK_STOPPED:
		return "stopped";
	case SMART_IO_TASK_TRACED:
		return "traced";
	case SMART_IO_TASK_DEAD:
		return "dead";
	case SMART_IO_TASK_ZOMBIE:
		return "zombie";
	case SMART_IO_TASK_PARKED:
		return "parked";
	case SMART_IO_TASK_IDLE:
		return "idle";
	default:
		return "unknown";
	}
}

bool smart_io_snapshot_task_state(pid_t tid, unsigned long *state_out, int *cpuid_out, u64 *u_s_time_out)
{
	struct task_struct *task;
	unsigned long state;

	if (tid <= 0)
		return false;

	rcu_read_lock();
	task = find_task_by_vpid(tid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return false;

	state = READ_ONCE(task->__state);
	state |= READ_ONCE(task->exit_state);
	if (state_out)
		*state_out = state;
	if (cpuid_out)
		*cpuid_out = task_cpu(task);
	if (u_s_time_out)
		*u_s_time_out = task->utime + task->stime;
	put_task_struct(task);
	return true;
}