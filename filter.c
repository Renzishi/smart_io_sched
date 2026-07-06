#include <linux/errno.h>
#include <linux/moduleparam.h>

#include "filter.h"
#include "smart_io_policy.h"

struct smart_io_policy current_policy = {
	.enabled = ATOMIC_INIT(0),
	.sample_rate_pct = ATOMIC_INIT(100),
	.match_uid = ATOMIC_INIT(-1),
	.only_sync = ATOMIC_INIT(0),
	.capture_scene_mask = ATOMIC_INIT(0),
};

int smart_io_policy_set(u32 rate, int uid, bool sync_only, u8 mask)
{
	if (rate > 100)
		return -EINVAL;
	atomic_set(&current_policy.sample_rate_pct, rate);
	atomic_set(&current_policy.match_uid, uid);
	atomic_set(&current_policy.only_sync, sync_only ? 1 : 0);
	atomic_set(&current_policy.capture_scene_mask, mask);
	return 0;
}