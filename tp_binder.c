#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/user_namespace.h>

#include <uapi/linux/android/binder.h>

#include "smart_io_log.h"
#include "system_context.h"
#include "trace_instance.h"

#define BINDER_PENDING_HASH_BITS 8
#define BINDER_ACTIVE_HASH_BITS 8

extern atomic_t smart_io_enabled;
extern atomic_t rawdata_trace_enabled;

struct binder_transaction;

struct binder_pending_txn {
	struct hlist_node node;
	struct binder_transaction *txn;
	struct task_struct *caller_task;
	u32 caller_uid;
};

struct binder_active_txn {
	struct hlist_node node;
	struct task_struct *caller_proc_task;
	struct task_struct *target_proc_task;
	struct task_struct *caller_thread_task;
	struct task_struct *target_thread_task;
	u32 caller_uid;
	u32 target_uid;
	pid_t caller_pid;
	pid_t caller_tid;
	pid_t target_pid;
	pid_t target_tid;
	int node_debug_id;
	u64 start_ts_ns;
};

static DEFINE_HASHTABLE(binder_pending_hash, BINDER_PENDING_HASH_BITS);
static DEFINE_HASHTABLE(binder_active_hash, BINDER_ACTIVE_HASH_BITS);
static DEFINE_SPINLOCK(binder_pending_lock);
static DEFINE_SPINLOCK(binder_active_lock);

struct tracepoints_table {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static inline struct task_struct *proc_task_of(struct task_struct *task)
{
	return task ? task->group_leader : NULL;
}

static inline u32 task_uid_nr_safe(const struct task_struct *task)
{
	if (!task)
		return (u32)-1;

	return from_kuid(&init_user_ns, task_uid(task));
}

static inline unsigned long binder_pending_hash_key(const struct binder_transaction *txn)
{
	return (unsigned long)txn;
}

static inline unsigned long binder_active_hash_key(struct task_struct *caller_proc_task,
						      struct task_struct *target_proc_task)
{
	return ((unsigned long)caller_proc_task >> 4) ^
	       ((unsigned long)target_proc_task >> 4);
}

static inline bool binder_raw_trace_enabled(void)
{
	return atomic_read(&smart_io_enabled) != 0 &&
	       atomic_read(&rawdata_trace_enabled) != 0;
}

static void binder_pending_release(struct binder_pending_txn *pending)
{
	if (!pending)
		return;

	if (pending->caller_task)
		put_task_struct(pending->caller_task);
	kfree(pending);
}

static void binder_active_release(struct binder_active_txn *active)
{
	if (!active)
		return;

	if (active->target_thread_task)
		put_task_struct(active->target_thread_task);
	if (active->caller_thread_task)
		put_task_struct(active->caller_thread_task);
	if (active->target_proc_task)
		put_task_struct(active->target_proc_task);
	if (active->caller_proc_task)
		put_task_struct(active->caller_proc_task);
	kfree(active);
}

static struct binder_pending_txn *binder_pending_take(struct binder_transaction *txn)
{
	struct binder_pending_txn *pending;
	struct binder_pending_txn *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&binder_pending_lock, flags);
	hash_for_each_possible(binder_pending_hash, pending, node,
			       binder_pending_hash_key(txn)) {
		if (pending->txn != txn)
			continue;
		hash_del(&pending->node);
		found = pending;
		break;
	}
	spin_unlock_irqrestore(&binder_pending_lock, flags);

	return found;
}

static void binder_pending_add(struct binder_transaction *txn,
			       struct task_struct *caller_task, u32 caller_uid)
{
	struct binder_pending_txn *pending;
	struct binder_pending_txn *entry;
	struct binder_pending_txn *stale = NULL;
	unsigned long flags;

	pending = kzalloc(sizeof(*pending), GFP_ATOMIC);
	if (!pending)
		return;

	get_task_struct(caller_task);
	pending->txn = txn;
	pending->caller_task = caller_task;
	pending->caller_uid = caller_uid;

	spin_lock_irqsave(&binder_pending_lock, flags);
	hash_for_each_possible(binder_pending_hash, entry, node,
			       binder_pending_hash_key(txn)) {
		if (entry->txn != txn)
			continue;
		hash_del(&entry->node);
		stale = entry;
		break;
	}
	hash_add(binder_pending_hash, &pending->node, binder_pending_hash_key(txn));
	spin_unlock_irqrestore(&binder_pending_lock, flags);

	binder_pending_release(stale);
}

static void binder_active_add(struct binder_active_txn *active)
{
	struct binder_active_txn *entry;
	struct binder_active_txn *stale = NULL;
	unsigned long flags;
	unsigned long key;

	key = binder_active_hash_key(active->caller_proc_task, active->target_proc_task);

	spin_lock_irqsave(&binder_active_lock, flags);
	hash_for_each_possible(binder_active_hash, entry, node, key) {
		if (entry->caller_proc_task != active->caller_proc_task ||
		    entry->target_proc_task != active->target_proc_task ||
		    entry->caller_thread_task != active->caller_thread_task)
			continue;
		hash_del(&entry->node);
		stale = entry;
		break;
	}
	hash_add(binder_active_hash, &active->node, key);
	spin_unlock_irqrestore(&binder_active_lock, flags);

	binder_active_release(stale);
}

static struct binder_active_txn *binder_active_take(struct task_struct *caller_proc_task,
						    struct task_struct *target_proc_task,
						    struct task_struct *caller_thread_task,
						    struct task_struct *target_thread_task)
{
	struct binder_active_txn *entry;
	struct binder_active_txn *found = NULL;
	unsigned long flags;
	unsigned long key;

	key = binder_active_hash_key(caller_proc_task, target_proc_task);

	spin_lock_irqsave(&binder_active_lock, flags);
	hash_for_each_possible(binder_active_hash, entry, node, key) {
		if (entry->caller_proc_task != caller_proc_task ||
		    entry->target_proc_task != target_proc_task ||
		    entry->caller_thread_task != caller_thread_task)
			continue;
		if (entry->target_thread_task &&
		    target_thread_task &&
		    entry->target_thread_task != target_thread_task)
			continue;
		hash_del(&entry->node);
		found = entry;
		break;
	}
	spin_unlock_irqrestore(&binder_active_lock, flags);

	return found;
}

static void cleanup_binder_state(void)
{
	struct binder_pending_txn *pending;
	struct binder_active_txn *active;
	struct hlist_node *tmp;
	int bucket;

	hash_for_each_safe(binder_pending_hash, bucket, tmp, pending, node) {
		hash_del(&pending->node);
		binder_pending_release(pending);
	}

	hash_for_each_safe(binder_active_hash, bucket, tmp, active, node) {
		hash_del(&active->node);
		binder_active_release(active);
	}
}

static void tp_binder_transaction_record_cb(void *ignore,
					    struct binder_transaction_data *tr,
					    struct binder_transaction *t,
					    struct binder_transaction *in_reply_to)
{
	struct task_struct *caller_task = current;
	uid_t fg_uid;
	u32 caller_uid;

	(void)ignore;

	if (!tr || !t || in_reply_to)
		return;
	if (tr->flags & TF_ONE_WAY)
		return;
	if (!binder_raw_trace_enabled())
		return;

	fg_uid = (uid_t)smart_io_get_fg_uid();
	if ((int)fg_uid < 0)
		return;

	caller_uid = task_uid_nr_safe(caller_task);
	if (caller_uid != fg_uid)
		return;

	binder_pending_add(t, caller_task, caller_uid);
}

static void tp_binder_proc_transaction_cb(void *ignore,
					  struct task_struct *caller_task,
					  struct task_struct *binder_proc_task,
					  struct task_struct *binder_th_task,
					  int node_debug_id,
					  struct binder_transaction *t,
					  bool pending_async)
{
	struct binder_pending_txn *pending;
	struct binder_active_txn *active;
	struct task_struct *caller_proc_task;
	u64 now;

	(void)ignore;

	if (!caller_task || !binder_proc_task || !t || pending_async)
		return;

	pending = binder_pending_take(t);
	if (!pending)
		return;

	if (!binder_raw_trace_enabled()) {
		binder_pending_release(pending);
		return;
	}

	if (pending->caller_task != caller_task) {
		binder_pending_release(pending);
		return;
	}

	caller_proc_task = proc_task_of(caller_task);
	if (!caller_proc_task) {
		binder_pending_release(pending);
		return;
	}

	active = kzalloc(sizeof(*active), GFP_ATOMIC);
	if (!active) {
		binder_pending_release(pending);
		return;
	}

	get_task_struct(caller_proc_task);
	get_task_struct(binder_proc_task);
	get_task_struct(caller_task);
	if (binder_th_task)
		get_task_struct(binder_th_task);

	now = ktime_get_boottime_ns();
	active->caller_proc_task = caller_proc_task;
	active->target_proc_task = binder_proc_task;
	active->caller_thread_task = caller_task;
	active->target_thread_task = binder_th_task;
	active->caller_uid = pending->caller_uid;
	active->target_uid = task_uid_nr_safe(binder_proc_task);
	active->caller_pid = task_tgid_nr(caller_task);
	active->caller_tid = task_pid_nr(caller_task);
	active->target_pid = task_tgid_nr(binder_proc_task);
	active->target_tid = binder_th_task ? task_pid_nr(binder_th_task) : 0;
	active->node_debug_id = node_debug_id;
	active->start_ts_ns = now;

	binder_active_add(active);
	binder_pending_release(pending);

	smart_io_raw_emit(
		"binder_sync_wait_begin: ts=%llu caller_uid=%u caller_pid=%d caller_tid=%d "
		"target_uid=%u target_pid=%d target_tid=%d node_debug_id=%d sync=1\n",
		now, active->caller_uid, active->caller_pid, active->caller_tid,
		active->target_uid, active->target_pid, active->target_tid,
		active->node_debug_id);
}

static void tp_sync_txn_recvd_cb(void *ignore, struct task_struct *tsk,
				 struct task_struct *from)
{
	struct task_struct *caller_proc_task;
	struct task_struct *target_proc_task;
	struct binder_active_txn *active;
	u64 now;
	u64 dur_us;

	(void)ignore;

	if (!tsk || !from)
		return;

	caller_proc_task = proc_task_of(tsk);
	target_proc_task = proc_task_of(from);
	if (!caller_proc_task || !target_proc_task)
		return;

	active = binder_active_take(caller_proc_task, target_proc_task, tsk, from);
	if (!active)
		return;

	if (!binder_raw_trace_enabled()) {
		binder_active_release(active);
		return;
	}

	now = ktime_get_boottime_ns();
	dur_us = div_u64(now - active->start_ts_ns, NSEC_PER_USEC);

	smart_io_raw_emit(
		"binder_sync_wait_end: ts=%llu dur_us=%llu caller_uid=%u caller_pid=%d caller_tid=%d "
		"target_uid=%u target_pid=%d target_tid=%d node_debug_id=%d sync=1\n",
		now, dur_us, active->caller_uid, active->caller_pid,
		active->caller_tid, task_uid_nr_safe(target_proc_task),
		task_tgid_nr(target_proc_task), task_pid_nr(from),
		active->node_debug_id);

	binder_active_release(active);
}

static struct tracepoints_table interests[] = {
	{ .name = "android_vh_binder_transaction_record",
	  .func = tp_binder_transaction_record_cb },
	{ .name = "android_vh_binder_proc_transaction",
	  .func = tp_binder_proc_transaction_cb },
	{ .name = "android_vh_sync_txn_recvd",
	  .func = tp_sync_txn_recvd_cb },
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
	cleanup_binder_state();
	smart_io_log_info("binder tracepoints unregistered.\n");
}

int register_binder_tracepoints(void)
{
	int i;
	int ret;

	cleanup_binder_state();
	hash_init(binder_pending_hash);
	hash_init(binder_active_hash);
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
