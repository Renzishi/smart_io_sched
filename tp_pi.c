#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/rtmutex.h>
#include <linux/tracepoint.h>
#include <linux/user_namespace.h>
#include <trace/hooks/dtask.h>

#include "smart_io_log.h"
#include "system_context.h"
#include "tp_pi.h"
#include "trace_instance.h"
#include "tool.h"

static atomic_t smart_io_pi_owner_pid = ATOMIC_INIT(0);

static struct task_struct *smart_io_pi_mutex_owner(struct mutex *lock)
{
	unsigned long owner;

	owner = atomic_long_read(&lock->owner);
	return (struct task_struct *)(owner & ~0x7UL);
}

static struct task_struct *smart_io_pi_rtmutex_owner(
		struct rt_mutex_base *lock)
{
	unsigned long owner;

	owner = (unsigned long)READ_ONCE(lock->owner);
	return (struct task_struct *)(owner & ~1UL);
}

static void smart_io_pi_trace_set(pid_t old_pid, pid_t new_pid,
				  const char *type)
{
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("pi_owner_set: ts=%llu old_pid=%d new_pid=%d type=%s\n",
			  ktime_get_boottime_ns(), old_pid, new_pid, type);
}

static void smart_io_pi_trace_clear(pid_t old_pid, const char *reason)
{
	if (old_pid == 0 || unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("pi_owner_clear: ts=%llu owner_pid=%d reason=%s\n",
			  ktime_get_boottime_ns(), old_pid, reason);
}

static void smart_io_pi_set_owner(struct task_struct *owner,
				  const char *type)
{
	pid_t old_pid;

	if (!owner || owner == current)
		return;

	old_pid = atomic_xchg(&smart_io_pi_owner_pid, owner->pid);
	smart_io_pi_trace_set(old_pid, owner->pid, type);
}

void smart_io_pi_clear_owner(const char *reason)
{
	pid_t old_pid;

	old_pid = atomic_xchg(&smart_io_pi_owner_pid, 0);
	smart_io_pi_trace_clear(old_pid, reason);
}

static void smart_io_pi_mutex_start(void *ignore, struct mutex *lock)
{
	struct task_struct *owner;

	(void)ignore;
	if (!current_is_fg_main())
		return;

	owner = smart_io_pi_mutex_owner(lock);
	smart_io_pi_set_owner(owner, "mutex");
}

static void smart_io_pi_mutex_finish(void *ignore, struct mutex *lock)
{
	(void)ignore;
	(void)lock;
	if (current_is_fg_main())
		smart_io_pi_clear_owner("mutex_wait_finish");
}

static void smart_io_pi_rtmutex_start(void *ignore,
					struct rt_mutex_base *lock)
{
	struct task_struct *owner;

	(void)ignore;
	if (!current_is_fg_main())
		return;

	owner = smart_io_pi_rtmutex_owner(lock);
	smart_io_pi_set_owner(owner, "rtmutex");
}

static void smart_io_pi_mutex_unlock(void *ignore, struct mutex *lock)
{
	(void)ignore;
	(void)lock;
	if (current->pid == atomic_read(&smart_io_pi_owner_pid))
		smart_io_pi_clear_owner("mutex_unlock");
}

bool smart_io_pi_current_marked(void)
{
	return current->pid == atomic_read(&smart_io_pi_owner_pid);
}

int register_pi_tracepoints(void)
{
	int ret;

	ret = register_trace_android_vh_mutex_wait_start(
		smart_io_pi_mutex_start, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_mutex_wait_finish(
		smart_io_pi_mutex_finish, NULL);
	if (ret)
		goto err_mutex_start;
	ret = register_trace_android_vh_mutex_unlock_slowpath(
		smart_io_pi_mutex_unlock, NULL);
	if (ret)
		goto err_mutex_finish;
	ret = register_trace_android_vh_rtmutex_wait_start(
		smart_io_pi_rtmutex_start, NULL);
	if (ret)
		goto err_mutex_unlock;
	return 0;

err_mutex_unlock:
	unregister_trace_android_vh_mutex_unlock_slowpath(
		smart_io_pi_mutex_unlock, NULL);
err_mutex_finish:
	unregister_trace_android_vh_mutex_wait_finish(
		smart_io_pi_mutex_finish, NULL);
err_mutex_start:
	unregister_trace_android_vh_mutex_wait_start(
		smart_io_pi_mutex_start, NULL);
	tracepoint_synchronize_unregister();
	return ret;
}

void unregister_pi_tracepoints(void)
{
	unregister_trace_android_vh_rtmutex_wait_start(
		smart_io_pi_rtmutex_start, NULL);
	unregister_trace_android_vh_mutex_unlock_slowpath(
		smart_io_pi_mutex_unlock, NULL);
	unregister_trace_android_vh_mutex_wait_finish(
		smart_io_pi_mutex_finish, NULL);
	unregister_trace_android_vh_mutex_wait_start(
		smart_io_pi_mutex_start, NULL);
	tracepoint_synchronize_unregister();
	smart_io_pi_clear_owner("tracepoint_unregister");
}
