#ifndef SMART_IO_POLICY_H
#define SMART_IO_POLICY_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/random.h>
#include <linux/types.h>

struct smart_io_policy {
	atomic_t enabled;
	atomic_t sample_rate_pct;
	atomic_t match_uid;
	atomic_t only_sync;
	atomic_t capture_scene_mask;
};

extern struct smart_io_policy current_policy;

static inline bool policy_should_capture(uid_t uid, bool is_sync, u32 scene)
{
	int match_uid;
	u32 rate;
	u32 mask;

	if (!atomic_read(&current_policy.enabled))
		return false;

	match_uid = atomic_read(&current_policy.match_uid);
	if (match_uid >= 0 && (uid_t)match_uid != uid)
		return false;
	if (atomic_read(&current_policy.only_sync) && !is_sync)
		return false;

	mask = (u32)atomic_read(&current_policy.capture_scene_mask);
	if (mask && scene < 32 && !(mask & BIT(scene)))
		return false;

	rate = (u32)atomic_read(&current_policy.sample_rate_pct);
	if (rate < 100 && (get_random_u32() % 100) >= rate)
		return false;

	return true;
}

#endif