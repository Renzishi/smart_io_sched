#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/user_namespace.h>

#include <uapi/linux/android/binder.h>

#include "smart_io_log.h"
#include "system_context.h"
#include "tp_binder.h"
#include "trace_instance.h"

#define BINDER_PI_SLOTS 8
#define BINDER_PI_MAX_DEPTH 3
#define BINDER_PI_TTL_NS (100ULL * NSEC_PER_MSEC)

extern atomic_t smart_io_enabled;
extern atomic_t rawdata_trace_enabled;

struct binder_transaction;

struct binder_pi_entry {
	pid_t pid;
	u8 depth;
	u64 expire_ns;
};

static struct binder_pi_entry binder_pi_slots[BINDER_PI_SLOTS];
static DEFINE_SPINLOCK(binder_pi_lock);
static atomic_t binder_pi_active = ATOMIC_INIT(0);

struct tracepoints_table {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static inline u32 task_uid_nr_safe(const struct task_struct *task)
{
	if (!task)
		return (u32)-1;

	return from_kuid(&init_user_ns, task_uid(task));
}

static bool task_is_fg_main(struct task_struct *task)
{
	int fg_uid = smart_io_get_fg_uid();

	if (!task || fg_uid < 0)
		return false;

	return task_uid_nr_safe(task) == (u32)fg_uid &&
	       task->pid == task->tgid;
}

static bool binder_pi_raw_enabled(void)
{
	return atomic_read(&smart_io_enabled) != 0 &&
	       atomic_read(&rawdata_trace_enabled) != 0;
}

static void binder_pi_purge_locked(u64 now)
{
	int i;

	for (i = 0; i < BINDER_PI_SLOTS; i++) {
		struct binder_pi_entry *entry = &binder_pi_slots[i];

		if (!entry->pid || entry->expire_ns > now)
			continue;

		entry->pid = 0;
		entry->depth = 0;
		entry->expire_ns = 0;
		atomic_dec(&binder_pi_active);
	}
}

static void binder_pi_mark(pid_t pid, const char *comm, u8 depth, u64 now)
{
	struct binder_pi_entry *entry;
	unsigned long flags;
	u64 expire_ns;
	u8 logged_depth;
	int free_idx = -1;
	int i;

	if (!pid || depth == 0 || depth > BINDER_PI_MAX_DEPTH)
		return;

	expire_ns = now + BINDER_PI_TTL_NS;

	spin_lock_irqsave(&binder_pi_lock, flags);
	binder_pi_purge_locked(now);

	for (i = 0; i < BINDER_PI_SLOTS; i++) {
		entry = &binder_pi_slots[i];
		if (entry->pid == pid) {
			if (depth < entry->depth)
				entry->depth = depth;
			entry->expire_ns = expire_ns;
			logged_depth = entry->depth;
			spin_unlock_irqrestore(&binder_pi_lock, flags);

			if (binder_pi_raw_enabled())
				smart_io_raw_emit("binder_pi_mark: ts=%llu pid=%d comm=%s depth=%u refresh=1\n",
						  now, pid, comm, logged_depth);
			return;
		}
		if (!entry->pid && free_idx < 0)
			free_idx = i;
	}

	if (free_idx < 0) {
		spin_unlock_irqrestore(&binder_pi_lock, flags);
		return;
	}

	entry = &binder_pi_slots[free_idx];
	entry->pid = pid;
	entry->depth = depth;
	entry->expire_ns = expire_ns;
	atomic_inc(&binder_pi_active);
	spin_unlock_irqrestore(&binder_pi_lock, flags);

	if (binder_pi_raw_enabled())
		smart_io_raw_emit("binder_pi_mark: ts=%llu pid=%d comm=%s depth=%u expire_ns=%llu\n",
				  now, pid, comm, depth, expire_ns);
}

static u8 binder_pi_depth_at(pid_t pid, u64 now)
{
	unsigned long flags;
	u8 depth = 0;
	int i;

	if (!pid || atomic_read(&binder_pi_active) == 0)
		return 0;

	spin_lock_irqsave(&binder_pi_lock, flags);
	binder_pi_purge_locked(now);

	for (i = 0; i < BINDER_PI_SLOTS; i++) {
		if (binder_pi_slots[i].pid == pid) {
			depth = binder_pi_slots[i].depth;
			break;
		}
	}
	spin_unlock_irqrestore(&binder_pi_lock, flags);

	return depth;
}

static void binder_pi_clear_pid(pid_t pid, const char *comm,
				const char *reason)
{
	struct binder_pi_entry *entry;
	unsigned long flags;
	int i;

	if (!pid || atomic_read(&binder_pi_active) == 0)
		return;

	spin_lock_irqsave(&binder_pi_lock, flags);
	for (i = 0; i < BINDER_PI_SLOTS; i++) {
		entry = &binder_pi_slots[i];
		if (entry->pid != pid)
			continue;

		entry->pid = 0;
		entry->depth = 0;
		entry->expire_ns = 0;
		atomic_dec(&binder_pi_active);
		spin_unlock_irqrestore(&binder_pi_lock, flags);

		if (binder_pi_raw_enabled())
			smart_io_raw_emit("binder_pi_clear: ts=%llu pid=%d comm=%s reason=%s\n",
					  ktime_get_boottime_ns(), pid, comm, reason);
		return;
	}
	spin_unlock_irqrestore(&binder_pi_lock, flags);
}

bool smart_io_binder_pi_current_marked(void)
{
	return binder_pi_depth_at(current->pid,
				  ktime_get_boottime_ns()) != 0;
}

void smart_io_binder_pi_clear_all(const char *reason)
{
	unsigned long flags;
	int active;
	int i;

	spin_lock_irqsave(&binder_pi_lock, flags);
	for (i = 0; i < BINDER_PI_SLOTS; i++) {
		binder_pi_slots[i].pid = 0;
		binder_pi_slots[i].depth = 0;
		binder_pi_slots[i].expire_ns = 0;
	}
	active = atomic_xchg(&binder_pi_active, 0);
	spin_unlock_irqrestore(&binder_pi_lock, flags);

	if (active && binder_pi_raw_enabled())
		smart_io_raw_emit("binder_pi_clear_all: ts=%llu active=%d reason=%s\n",
				  ktime_get_boottime_ns(), active, reason);
}

static void tp_sync_txn_recvd_cb(void *ignore, struct task_struct *tsk,
				 struct task_struct *from)
{
	u64 now;
	u8 depth;

	(void)ignore;

	if (!tsk || !from || tsk == from)
		return;
	if (atomic_read(&smart_io_enabled) == 0)
		return;

	now = ktime_get_boottime_ns();

	if (task_is_fg_main(from)) {
		binder_pi_mark(tsk->pid, tsk->comm, 1, now);
		return;
	}

	depth = binder_pi_depth_at(from->pid, now);
	if (depth && depth < BINDER_PI_MAX_DEPTH)
		binder_pi_mark(tsk->pid, tsk->comm, depth + 1, now);
}

static void tp_binder_transaction_record_cb(void *ignore,
					    struct binder_transaction_data *tr,
					    struct binder_transaction *t,
					    struct binder_transaction *in_reply_to)
{
	(void)ignore;
	(void)tr;
	(void)t;

	if (!in_reply_to)
		return;
	if (atomic_read(&binder_pi_active) == 0)
		return;

	binder_pi_clear_pid(current->pid, current->comm, "reply");
}

static struct tracepoints_table interests[] = {
	{ .name = "android_vh_sync_txn_recvd",
	  .func = tp_sync_txn_recvd_cb },
	{ .name = "android_vh_binder_transaction_record",
	  .func = tp_binder_transaction_record_cb },
};

#define FOR_EACH_INTEREST(i) \
	for (i = 0; i < ARRAY_SIZE(interests); i++)

static void lookup_tracepoints(struct tracepoint *tp, void *ignore)
{
	int i;

	(void)ignore;

	FOR_EACH_INTEREST(i) {
		if (strcmp(interests[i].name, tp->name) == 0)
			interests[i].tp = tp;
	}
}

void unregister_binder_tracepoints(void)
{
	int i;

	FOR_EACH_INTEREST(i) {
		if (interests[i].init) {
			tracepoint_probe_unregister(interests[i].tp,
						    interests[i].func, NULL);
			interests[i].init = false;
		}
	}

	tracepoint_synchronize_unregister();
	smart_io_binder_pi_clear_all("unregister");
	smart_io_log_info("binder tracepoints unregistered.\n");
}

int register_binder_tracepoints(void)
{
	int i;
	int ret;

	smart_io_binder_pi_clear_all("register");
	for_each_kernel_tracepoint(lookup_tracepoints, NULL);

	FOR_EACH_INTEREST(i) {
		if (!interests[i].tp) {
			smart_io_log_err("tracepoint %s not found\n",
					 interests[i].name);
			unregister_binder_tracepoints();
			return -EINVAL;
		}

		ret = tracepoint_probe_register(interests[i].tp,
						interests[i].func, NULL);
		if (ret) {
			smart_io_log_err("failed to register %s: %d\n",
					 interests[i].name, ret);
			unregister_binder_tracepoints();
			return ret;
		}

		interests[i].init = true;
		smart_io_log_info("tracepoint %s registered.\n",
				  interests[i].name);
	}

	return 0;
}
