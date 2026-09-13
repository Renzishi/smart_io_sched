#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include "smart_io_log.h"
#include "system_context.h"
#include "tp_sched.h"
#include "trace_instance.h"
#include "tool.h"

static bool fg_main_pid_matches(pid_t pid)
{
	pid_t fg_pid;

	if (atomic_read(&rawdata_trace_enabled) == 0 || pid <= 0)
		return false;

	fg_pid = (pid_t)smart_io_get_fg_main_pid();
	return fg_pid > 0 && fg_pid == pid;
}

static void tp_sched_switch_cb(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next,
			       unsigned int prev_state)
{
	u32 prev_state_enum;

	(void)ignore;
	(void)preempt;

	if (!prev || !next)
		return;
	/* Only log when the fg main thread gives up the cpu because of a
	 * blocking state (S/D); preemption (prev_state == RUNNING) is skipped.
	 */
	if (!fg_main_pid_matches(prev->pid))
		return;
	if (!(prev_state & (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)))
		return;

	prev_state_enum = smart_io_task_state_to_enum(prev_state);

	smart_io_raw_emit("sched_switch: ts=%llu "
		"p_pid=%d p_comm=%s p_state=%u p_state_s=%s "
		"n_pid=%d n_comm=%s n_state=%u n_state_s=%s\n",
			  ktime_get_boottime_ns(), prev->pid, prev->comm,
			  prev_state_enum, smart_io_task_state_to_str(prev_state_enum),
			  next->pid, next->comm, SMART_IO_TASK_RUNNING,
			  smart_io_task_state_to_str(SMART_IO_TASK_RUNNING));
}

static void tp_sched_wakeup_cb(void *ignore, struct task_struct *task)
{
	(void)ignore;

	/* task is the woken fg main thread; current is the waker context.
	 * (The kernel only passes the woken task to sched_wakeup; current is
	 * the best approximation of the waker. It equals task on self-wakeup.)
	 */
	if (!task || !fg_main_pid_matches(task->pid))
		return;

	smart_io_raw_emit("sched_wakeup: ts=%llu "
		"p_pid=%d p_comm=%s n_pid=%d n_comm=%s\n",
			  ktime_get_boottime_ns(), current->pid, current->comm,
			  task->pid, task->comm);
}

struct sched_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct sched_tracepoint_entry sched_tps[] = {
	{ .name = "sched_switch", .func = tp_sched_switch_cb },
	{ .name = "sched_wakeup", .func = tp_sched_wakeup_cb },
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
