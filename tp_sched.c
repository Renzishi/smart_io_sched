#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/hardirq.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/user_namespace.h>

#include "smart_io_log.h"
#include "smart_io_rl.h"
#include "system_context.h"
#include "tp_sched.h"
#include "trace_instance.h"
#include "tool.h"

/*
 * Main-thread (foreground app main thread) run-state observation for State v2
 * (RL 晋升四目标方案 §B2). The main thread is the thread of the foreground
 * UID whose pid == tgid (the thread-group leader); fg_main_pid is never used
 * for identity (it is written late/stale by the Android framework layer).
 *
 * Two independent concerns:
 *  1. Tracking for the live RL State: the block (S/D) interval start is
 *     recorded from the switch-out event and cleared on wakeup/switch-in.
 *     This runs whether or not raw printing is enabled, because the online
 *     State must keep updating while RL is used.
 *  2. Raw printing (gated by rawdata_trace): sched_switch out/in and
 *     sched_waking events with the fields the offline parser needs to
 *     reconstruct T_S/T_D/T_runnable. sched_waking is used (not sched_wakeup)
 *     because it is guaranteed to run in the waking context, so `current` is
 *     the waker; it also gives the correct wakeup timestamp for T_runnable
 *     (wakeup to switch-in).
 */
static bool fg_main_thread_matches(struct task_struct *task)
{
	int fg_uid;
	u32 uid;

	if (!task)
		return false;

	/*
	 * One foreground app has exactly one main thread: the thread-group
	 * leader (pid == tgid) of the foreground UID. fg_main_pid is
	 * intentionally not used here because the framework writes it late
	 * and it can be stale after a switch.
	 */
	fg_uid = smart_io_get_fg_uid();
	if (fg_uid < 0)
		return false;
	uid = from_kuid(&init_user_ns, task_uid(task));
	return fg_uid == uid && task->pid == task->tgid;
}

static void tp_sched_switch_cb(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next,
			       unsigned int prev_state)
{
	u32 prev_state_enum;
	bool prev_main;
	bool next_main;

	(void)ignore;
	if (!prev || !next)
		return;

	prev_main = fg_main_thread_matches(prev);
	next_main = fg_main_thread_matches(next);
	prev_state_enum = smart_io_task_state_to_enum(prev_state);

	/*
	 * Track the main-thread block interval (always, not gated by print).
	 * switch-out into S/D starts the interval; switch-in (or a preempt
	 * out, prev_state RUNNING) ends/does-not-start it.
	 */
	if (prev_main)
		smart_io_rl_track_switch_out(prev_state, preempt);
	if (next_main)
		smart_io_rl_track_switch_in();

	if (!prev_main && !next_main)
		return;
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	/* switch-in: the fg main thread is scheduled (resume/切入). */
	if (next_main)
		smart_io_raw_emit("sched_switch: ts=%llu event=in "
				  "main_pid=%d p_pid=%d p_comm=%s n_pid=%d n_comm=%s\n",
				  ktime_get_boottime_ns(), next->pid,
				  prev->pid, prev->comm, next->pid, next->comm);

	/* switch-out for the fg main thread (S/D or preempt). */
	if (prev_main)
		smart_io_raw_emit("sched_switch: ts=%llu event=out "
				  "main_pid=%d p_comm=%s p_state=%u p_state_s=%s "
				  "preempt=%u in_iowait=%u "
				  "n_pid=%d n_comm=%s n_state=%u n_state_s=%s\n",
				  ktime_get_boottime_ns(), prev->pid, prev->comm,
				  prev_state_enum,
				  smart_io_task_state_to_str(prev_state_enum),
				  preempt ? 1U : 0U,
				  prev->in_iowait ? 1U : 0U,
				  next->pid, next->comm, SMART_IO_TASK_RUNNING,
				  smart_io_task_state_to_str(SMART_IO_TASK_RUNNING));
}

static void tp_sched_waking_cb(void *ignore, struct task_struct *task)
{
	(void)ignore;

	/* sched_waking runs in the waking context, so `current` is the waker. */
	if (!task || !fg_main_thread_matches(task))
		return;

	/* End the block interval (always, not gated by print). */
	smart_io_rl_track_wakeup();

	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("sched_wakeup: ts=%llu "
			  "target_pid=%d target_comm=%s "
			  "waker_pid=%d waker_tgid=%d waker_comm=%s "
			  "hardirq=%u serving_softirq=%u\n",
			  ktime_get_boottime_ns(),
			  task->pid, task->comm,
			  current->pid, current->tgid, current->comm,
			  in_hardirq() ? 1U : 0U,
			  in_serving_softirq() ? 1U : 0U);
}

struct sched_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct sched_tracepoint_entry sched_tps[] = {
	{ .name = "sched_switch", .func = tp_sched_switch_cb },
	{ .name = "sched_waking", .func = tp_sched_waking_cb },
};

#define FOR_EACH_SCHED_TP(i) \
	for (i = 0; i < ARRAY_SIZE(sched_tps); i++)

static void lookup_sched_tracepoints(struct tracepoint *tp, void *ignore)
{
	int i;

	(void)ignore;

	FOR_EACH_SCHED_TP(i) {
		if (!strcmp(sched_tps[i].name, tp->name))
			sched_tps[i].tp = tp;
	}
}

void unregister_sched_tracepoints(void)
{
	int i;

	FOR_EACH_SCHED_TP(i) {
		if (sched_tps[i].init) {
			tracepoint_probe_unregister(sched_tps[i].tp,
						    sched_tps[i].func, NULL);
			sched_tps[i].init = false;
		}
	}

	tracepoint_synchronize_unregister();
	smart_io_log_info("sched tracepoints unregistered.\n");
}

int register_sched_tracepoints(void)
{
	int i;
	int ret;

	for_each_kernel_tracepoint(lookup_sched_tracepoints, NULL);

	FOR_EACH_SCHED_TP(i) {
		if (!sched_tps[i].tp) {
			smart_io_log_warn("tracepoint %s not found, sched raw tracing disabled.\n",
					  sched_tps[i].name);
			unregister_sched_tracepoints();
			return -ENOENT;
		}

		ret = tracepoint_probe_register(sched_tps[i].tp,
						sched_tps[i].func, NULL);
		if (ret) {
			smart_io_log_warn("failed to register %s: %d, sched raw tracing disabled.\n",
					  sched_tps[i].name, ret);
			unregister_sched_tracepoints();
			return ret;
		}
		sched_tps[i].init = true;
		smart_io_log_info("tracepoint %s registered.\n", sched_tps[i].name);
	}

	return 0;
}
