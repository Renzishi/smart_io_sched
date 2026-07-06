#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/smp.h>
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
	if (!fg_main_pid_matches(prev->pid) && !fg_main_pid_matches(next->pid))
		return;

	prev_state_enum = smart_io_task_state_to_enum(prev_state);

	smart_io_raw_emit("sched_switch: ts=%llu "
		"prev_pid=%d prev_comm=%s prev_state=%u prev_state_s=%s "
		"next_pid=%d next_comm=%s next_state=%u next_state_s=%s cpu=%d\n",
			  ktime_get_boottime_ns(), prev->pid, prev->comm,
			  prev_state_enum, smart_io_task_state_to_str(prev_state_enum),
			  next->pid, next->comm, SMART_IO_TASK_RUNNING,
			  smart_io_task_state_to_str(SMART_IO_TASK_RUNNING),
			  raw_smp_processor_id());
}

static void tp_sched_wakeup_cb(void *ignore, struct task_struct *task)
{
	u32 state_enum;
	unsigned long raw_state = 0;

	(void)ignore;

	if (!task || !fg_main_pid_matches(task->pid))
		return;

	raw_state = READ_ONCE(task->__state);
	state_enum = smart_io_task_state_to_enum(raw_state);
	smart_io_raw_emit("sched_wakeup: ts=%llu "
		"pid=%d comm=%s target_cpu=%d "
		"state=%u state_s=%s raw_state=%lu\n",
			  ktime_get_boottime_ns(), task->pid, task->comm,
			  task_cpu(task), state_enum,
			  smart_io_task_state_to_str(state_enum), raw_state);
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
