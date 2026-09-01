// SPDX-License-Identifier: GPL-2.0
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/user_namespace.h>
#include <linux/wait.h>
#include <trace/hooks/mm.h>

#include "smart_io_dirtyrate.h"
#include "smart_io_log.h"
#include "system_context.h"

#define SMART_IO_DIRTYRATE_MAX_SLEEP_NS	NSEC_PER_SEC
#define SMART_IO_DIRTYRATE_KIB_BYTES	1024ULL
#define SMART_IO_DIRTYRATE_BACKGROUND_PATH	"/background"

static DEFINE_SPINLOCK(dirtyrate_lock);
static DECLARE_WAIT_QUEUE_HEAD(dirtyrate_waitq);
static atomic64_t dirtyrate_config_seq = ATOMIC64_INIT(1);
static bool dirtyrate_enabled;
static bool dirtyrate_active;
static u64 dirtyrate_config_wbps;
static u64 dirtyrate_wbps;
static u64 dirtyrate_burst_bytes;
static u64 dirtyrate_tokens;
static u64 dirtyrate_last_refill_ns;
static u64 dirtyrate_charged_bytes;
static u64 dirtyrate_wait_count;
static u64 dirtyrate_wait_ns;
static u64 dirtyrate_bypass_fg_count;
static u64 dirtyrate_bypass_invalid_fg_count;

static u64 dirtyrate_add_sat(u64 left, u64 right)
{
	if (U64_MAX - left < right)
		return U64_MAX;
	return left + right;
}

static u64 dirtyrate_bytes_for_ns(u64 duration_ns, u64 rate_bps)
{
	return mul_u64_u64_div_u64(duration_ns, rate_bps, NSEC_PER_SEC);
}

static u64 dirtyrate_ns_for_bytes(u64 bytes, u64 rate_bps, bool round_up)
{
	u64 remainder;
	u64 seconds;
	u64 duration_ns;
	u64 partial_ns;

	seconds = div64_u64_rem(bytes, rate_bps, &remainder);
	if (seconds > U64_MAX / NSEC_PER_SEC)
		return U64_MAX;

	duration_ns = seconds * NSEC_PER_SEC;
	partial_ns = mul_u64_u64_div_u64(remainder, NSEC_PER_SEC, rate_bps);
	duration_ns = dirtyrate_add_sat(duration_ns, partial_ns);
	if (round_up && remainder)
		duration_ns = dirtyrate_add_sat(duration_ns, 1);

	return duration_ns;
}

static void dirtyrate_reset_bucket_locked(u64 wbps, u64 now_ns)
{
	dirtyrate_wbps = wbps;
	dirtyrate_burst_bytes = wbps ?
		max_t(u64, PAGE_SIZE, wbps / 8) : 0;
	dirtyrate_tokens = dirtyrate_burst_bytes;
	dirtyrate_last_refill_ns = now_ns;
}

static void dirtyrate_refill_locked(u64 now_ns)
{
	u64 added;
	u64 elapsed;
	u64 missing;
	u64 full_refill_ns;
	u64 advanced;

	if (!dirtyrate_wbps || !dirtyrate_burst_bytes)
		return;
	if (dirtyrate_tokens >= dirtyrate_burst_bytes) {
		dirtyrate_tokens = dirtyrate_burst_bytes;
		dirtyrate_last_refill_ns = now_ns;
		return;
	}
	if (now_ns <= dirtyrate_last_refill_ns)
		return;

	elapsed = now_ns - dirtyrate_last_refill_ns;
	missing = dirtyrate_burst_bytes - dirtyrate_tokens;
	full_refill_ns = dirtyrate_ns_for_bytes(missing, dirtyrate_wbps, true);
	if (elapsed >= full_refill_ns) {
		dirtyrate_tokens = dirtyrate_burst_bytes;
		dirtyrate_last_refill_ns = now_ns;
		return;
	}

	added = dirtyrate_bytes_for_ns(elapsed, dirtyrate_wbps);
	if (!added)
		return;
	if (added >= missing) {
		dirtyrate_tokens = dirtyrate_burst_bytes;
		dirtyrate_last_refill_ns = now_ns;
		return;
	}

	dirtyrate_tokens += added;
	advanced = dirtyrate_ns_for_bytes(added, dirtyrate_wbps, false);
	advanced = max_t(u64, advanced, 1);
	advanced = dirtyrate_add_sat(dirtyrate_last_refill_ns, advanced);
	dirtyrate_last_refill_ns = min_t(u64, now_ns, advanced);
}

static bool dirtyrate_wait_changed(u64 config_seq, u32 uid)
{
	int fg_uid = smart_io_get_fg_uid();

	return config_seq != atomic64_read(&dirtyrate_config_seq) ||
		!READ_ONCE(dirtyrate_enabled) || !READ_ONCE(dirtyrate_active) ||
		!READ_ONCE(dirtyrate_wbps) ||
		fg_uid < 0 || (u32)fg_uid == uid;
}

static bool dirtyrate_current_is_background(void)
{
	struct cgroup_subsys_state *css;
	char path[32];
	bool background = false;

	rcu_read_lock();
	css = task_css(current, io_cgrp_id);
	if (css && css->cgroup &&
	    !cgroup_path(css->cgroup, path, sizeof(path)))
		background = !strcmp(path, SMART_IO_DIRTYRATE_BACKGROUND_PATH);
	rcu_read_unlock();

	return background;
}

static void smart_io_dirtyrate_dirty_rate(void *unused, struct inode *inode)
{
	u32 uid;

	if (!inode || fatal_signal_pending(current))
		return;

	uid = from_kuid_munged(&init_user_ns, current_uid());
	for (;;) {
		unsigned long flags;
		u64 now_ns;
		u64 wait_ns;
		u64 config_seq;
		u64 wait_start_ns;
		int fg_uid;
		int ret;

		fg_uid = smart_io_get_fg_uid();
		if (READ_ONCE(dirtyrate_enabled) &&
		    READ_ONCE(dirtyrate_active) &&
		    READ_ONCE(dirtyrate_wbps) && fg_uid >= 0 &&
		    (u32)fg_uid != uid && !dirtyrate_current_is_background())
			return;
		spin_lock_irqsave(&dirtyrate_lock, flags);
		if (!dirtyrate_enabled || !dirtyrate_active || !dirtyrate_wbps) {
			spin_unlock_irqrestore(&dirtyrate_lock, flags);
			return;
		}
		if (fg_uid < 0) {
			dirtyrate_bypass_invalid_fg_count = dirtyrate_add_sat(
				dirtyrate_bypass_invalid_fg_count, 1);
			spin_unlock_irqrestore(&dirtyrate_lock, flags);
			return;
		}
		if ((u32)fg_uid == uid) {
			dirtyrate_bypass_fg_count = dirtyrate_add_sat(
				dirtyrate_bypass_fg_count, 1);
			spin_unlock_irqrestore(&dirtyrate_lock, flags);
			return;
		}

		now_ns = ktime_get_ns();
		dirtyrate_refill_locked(now_ns);
		if (dirtyrate_tokens >= PAGE_SIZE) {
			dirtyrate_tokens -= PAGE_SIZE;
			dirtyrate_charged_bytes = dirtyrate_add_sat(
				dirtyrate_charged_bytes, PAGE_SIZE);
			spin_unlock_irqrestore(&dirtyrate_lock, flags);
			return;
		}

		wait_ns = dirtyrate_ns_for_bytes(PAGE_SIZE - dirtyrate_tokens,
						 dirtyrate_wbps, true);
		wait_ns = min_t(u64, wait_ns,
				SMART_IO_DIRTYRATE_MAX_SLEEP_NS);
		dirtyrate_wait_count = dirtyrate_add_sat(dirtyrate_wait_count, 1);
		config_seq = atomic64_read(&dirtyrate_config_seq);
		spin_unlock_irqrestore(&dirtyrate_lock, flags);

		wait_start_ns = ktime_get_ns();
		ret = wait_event_interruptible_hrtimeout(dirtyrate_waitq,
				dirtyrate_wait_changed(config_seq, uid),
				ns_to_ktime(wait_ns));
		spin_lock_irqsave(&dirtyrate_lock, flags);
		dirtyrate_wait_ns = dirtyrate_add_sat(dirtyrate_wait_ns,
						      ktime_get_ns() - wait_start_ns);
		spin_unlock_irqrestore(&dirtyrate_lock, flags);
		if (ret == -ERESTARTSYS || fatal_signal_pending(current))
			return;
	}
}

int smart_io_dirtyrate_init(void)
{
	int ret;

	if (trace_android_rvh_ctl_dirty_rate_enabled()) {
		smart_io_log_err("dirty-rate hook is already in use\n");
		return -EBUSY;
	}

	ret = register_trace_android_rvh_ctl_dirty_rate(
			smart_io_dirtyrate_dirty_rate, NULL);
	if (ret) {
		smart_io_log_err("dirty-rate hook registration failed: %d\n", ret);
		return ret;
	}

	smart_io_log_info("dirty-rate hook registered; module unload is disabled.\n");
	return 0;
}

void smart_io_dirtyrate_fg_uid_changed(void)
{
	atomic64_inc(&dirtyrate_config_seq);
	wake_up_all(&dirtyrate_waitq);
}

int smart_io_dirtyrate_set_enabled(bool enabled)
{
	unsigned long flags;

	spin_lock_irqsave(&dirtyrate_lock, flags);
	dirtyrate_enabled = enabled;
	dirtyrate_active = false;
	dirtyrate_reset_bucket_locked(0, ktime_get_ns());
	atomic64_inc(&dirtyrate_config_seq);
	spin_unlock_irqrestore(&dirtyrate_lock, flags);
	wake_up_all(&dirtyrate_waitq);
	return 0;
}

bool smart_io_dirtyrate_get_enabled(void)
{
	return READ_ONCE(dirtyrate_enabled);
}

int smart_io_dirtyrate_set_active(bool active)
{
	unsigned long flags;

	spin_lock_irqsave(&dirtyrate_lock, flags);
	if (!dirtyrate_enabled)
		active = false;
	if (dirtyrate_active == active)
		goto out;

	dirtyrate_active = active;
	dirtyrate_reset_bucket_locked(active ? dirtyrate_config_wbps : 0,
					      ktime_get_ns());
	atomic64_inc(&dirtyrate_config_seq);
out:
	spin_unlock_irqrestore(&dirtyrate_lock, flags);
	wake_up_all(&dirtyrate_waitq);
	return 0;
}

int smart_io_dirtyrate_set_wkbps(u64 wkbps)
{
	u64 wbps;
	unsigned long flags;

	if (wkbps > U64_MAX / SMART_IO_DIRTYRATE_KIB_BYTES)
		return -ERANGE;
	wbps = wkbps * SMART_IO_DIRTYRATE_KIB_BYTES;

	spin_lock_irqsave(&dirtyrate_lock, flags);
	dirtyrate_config_wbps = wbps;
	if (dirtyrate_active)
		dirtyrate_reset_bucket_locked(wbps, ktime_get_ns());
	atomic64_inc(&dirtyrate_config_seq);
	spin_unlock_irqrestore(&dirtyrate_lock, flags);
	wake_up_all(&dirtyrate_waitq);
	return 0;
}

u64 smart_io_dirtyrate_get_wkbps(void)
{
	return READ_ONCE(dirtyrate_config_wbps) / SMART_IO_DIRTYRATE_KIB_BYTES;
}
