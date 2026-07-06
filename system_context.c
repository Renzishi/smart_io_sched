#include <linux/atomic.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/vmstat.h>

#include "io_semantics.h"
#include "smart_io_log.h"
#include "system_context.h"
#include "tp_filemap.h"
#include "trace_instance.h"
#include "tool.h"

static struct task_struct *sys_poll_kthread;
static atomic_t fg_uid = ATOMIC_INIT(-1);
static atomic_t scene = ATOMIC_INIT(0);
static atomic_t fg_main_pid = ATOMIC_INIT(-1);
// static atomic64_t main_thread_last_u_s_time = ATOMIC64_INIT(0);
// static atomic64_t main_thread_last_sample_time = ATOMIC64_INIT(0);

static void emit_fg_main_pid_state(void)
{
	pid_t uid = (pid_t)atomic_read(&fg_uid);
	pid_t pid = (pid_t)atomic_read(&fg_main_pid);
	unsigned long raw_state = 0;
	u32 state_enum;
	int cpuid = -1;
	// u64 last_u_s_time = atomic64_read(&main_thread_last_u_s_time);
	u64 u_s_time = 0;
	u64 now = ktime_get_boottime_ns();

	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_snapshot_task_state(pid, &raw_state, &cpuid, &u_s_time);
	state_enum = smart_io_task_state_to_enum(raw_state);
	u32 mem_avail_mb = pages_to_mb(si_mem_available());
	u32 dirty_pages_mb = pages_to_mb(global_node_page_state(NR_FILE_DIRTY));
	u32 wb_pages_mb = pages_to_mb(global_node_page_state(NR_WRITEBACK));
	u32 swap_mb = get_swap_used_mb();
	// u32 run_pct = min_t(u32, div64_u64((u_s_time - last_u_s_time) * 100, now - atomic64_read(&main_thread_last_sample_time)), 100);
	// atomic64_set(&main_thread_last_u_s_time, u_s_time);
	// atomic64_set(&main_thread_last_sample_time, now);
	// smart_io_raw_emit("main_pid_state_sample: ts=%llu uid=%d pid=%d run_pct=%u state=%u state_s=%s cpuid=%d mem_avail_mb=%u dirty_pages_mb=%u wb_pages_mb=%u swap_mb=%u\n",
	// 		  now, uid, pid, run_pct, state_enum,
	// 		  smart_io_task_state_to_str(state_enum), cpuid, mem_avail_mb, dirty_pages_mb, wb_pages_mb, swap_mb);
	smart_io_raw_emit("main_pid_state_sample: ts=%llu uid=%d pid=%d state=%u state_s=%s cpuid=%d mem_avail_mb=%u dirty_pages_mb=%u wb_pages_mb=%u swap_mb=%u\n",
			  now, uid, pid, state_enum,
			  smart_io_task_state_to_str(state_enum), cpuid, mem_avail_mb, dirty_pages_mb, wb_pages_mb, swap_mb);
}

// void emit_cpufreq_util_sample(void)
// {
// 	u32 freq_khz;
// 	u32 max_khz;
// 	int cpu;

// 	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
// 		return;

// 	for_each_possible_cpu(cpu) {
// 		freq_khz = cpufreq_quick_get(cpu);
// 		if (unlikely(freq_khz == 0))
// 			continue;
// 		max_khz = cpufreq_quick_get_max(cpu);
// 		if (unlikely(max_khz == 0))
// 			continue;
// 		smart_io_raw_emit("cpufreq_util_sample: ts=%llu cpu=%d freq_khz=%u max_khz=%u\n",
// 				  ktime_get_boottime_ns(), cpu, freq_khz, max_khz);
// 	}
// }

static int sys_poll_thread(void *data)
{
	while (!kthread_should_stop()) {
		msleep_interruptible(get_window_sz_ms());
		smart_io_periodic_tick();
		emit_fg_main_pid_state();
		smart_io_filemap_agg_flush();
		// emit_cpufreq_util_sample();
	}
	return 0;
}

int smart_io_ctx_init(void)
{
	sys_poll_kthread = kthread_run(sys_poll_thread, NULL, "smart_io_poll");
	if (IS_ERR(sys_poll_kthread)) {
		int ret = PTR_ERR(sys_poll_kthread);

		sys_poll_kthread = NULL;
		smart_io_log_err("failed to start polling thread: %d\n", ret);
		return ret;
	}

	smart_io_log_info("polling thread started.\n");
	return 0;
}

void smart_io_ctx_exit(void)
{
	if (sys_poll_kthread)
		kthread_stop(sys_poll_kthread);
	sys_poll_kthread = NULL;
	smart_io_log_info("polling thread stopped.\n");
}

void smart_io_set_fg_uid(uid_t uid)
{
	if (uid == atomic_read(&fg_uid))
		return;
	atomic_set(&fg_uid, uid);
}

int smart_io_get_fg_uid(void)
{
	return atomic_read(&fg_uid);
}

void smart_io_set_scene_tag(u32 scene_tag)
{
	atomic_set(&scene, scene_tag);
}

void smart_io_get_fg_ctx(uid_t *uid, u32 *scene_tag)
{
	*uid = (uid_t)atomic_read(&fg_uid);
	*scene_tag = (u32)atomic_read(&scene);
}

void smart_io_set_fg_main_pid(pid_t pid)
{
	if (pid == atomic_read(&fg_main_pid))
		return;
	atomic_set(&fg_main_pid, pid);
	// u64 u_s_time = 0;
	// bool valid = smart_io_snapshot_task_state(pid, NULL, NULL, &u_s_time);
	// if (valid) {
	// 	atomic64_set(&main_thread_last_u_s_time, u_s_time);
	// 	atomic64_set(&main_thread_last_sample_time, ktime_get_boottime_ns());
	// }
	if (unlikely(atomic_read(&rawdata_trace_enabled) != 0)) {
		smart_io_raw_emit("fg_main_pid: ts=%llu pid=%d\n",
				  ktime_get_boottime_ns(), pid);
	}
}

int smart_io_get_fg_main_pid(void)
{
	return atomic_read(&fg_main_pid);
}
