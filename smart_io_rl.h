/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMART_IO_RL_H
#define SMART_IO_RL_H

#include <linux/types.h>

#define SMART_IO_RL_DEV_LAT_WINDOW 10U

struct request;

struct smart_io_rl_device_snapshot {
	u32 r_flt;
	u32 w_flt;
	u32 r_wait;
	u32 w_wait;
	u32 tag_used;
	u32 tag_max;
	u32 r_lat_us;
	u32 w_lat_us;
};

/*
 * READ request-level RL priority (v1).
 *
 * The RL decision runs in dd_insert_request(), before queue selection, and
 * writes the chosen action into the request ioprio hint field using bits 7-8.
 * Those bits are NOT part of SMART_IO_PRIVATE_HINT_MASK (the route bits used
 * by the 12-action throttle model, bits 3-6); they are only parsed offline
 * from the final ioprio recorded by block_rq_insert. See
 * docs/RL-per-request优先级-排采集前置设计稿.md for the full spec.
 *
 * The route/source hint bits (HP/PGF/PI/VIP/MAIN, bits 3-6 and 9) are NOT
 * defined here; they come from smart_io_types.h (SMART_IO_*_IOPRIO_HINT),
 * the module's single source of truth. This file only adds the RL-action
 * bits (7-8) on top.
 */

#include "smart_io_types.h"

#define SMART_IO_RL_ACTION_SHIFT 7
#define SMART_IO_RL_ACTION_MASK \
	(0x3U << SMART_IO_RL_ACTION_SHIFT)

#define SMART_IO_RL_ACTION_NONE  0U
#define SMART_IO_RL_ACTION_VIP   1U
#define SMART_IO_RL_ACTION_HP    2U
#define SMART_IO_RL_ACTION_RT    3U

/* Collection modes (doc §9): 0 off, A rule, B fixed, C random, D model. */
#define SMART_IO_RL_MODE_OFF    0U
#define SMART_IO_RL_MODE_RULE   1U
#define SMART_IO_RL_MODE_FIXED  2U
#define SMART_IO_RL_MODE_RANDOM 3U
#define SMART_IO_RL_MODE_MODEL  4U

#define SMART_IO_RL_MODE_MAX    4U

enum smart_io_rl_uid_class {
	SMART_IO_RL_UID_UNKNOWN = 0,
	SMART_IO_RL_UID_ROOT,
	SMART_IO_RL_UID_SYSTEM,
	SMART_IO_RL_UID_VENDOR,
	SMART_IO_RL_UID_SYSTEM_APP,
	SMART_IO_RL_UID_PREINSTALLED_APP,
	SMART_IO_RL_UID_NORMAL_APP,
	SMART_IO_RL_UID_ISOLATED,
};

enum smart_io_rl_cgroup_class {
	SMART_IO_RL_CGROUP_UNKNOWN = 0,
	SMART_IO_RL_CGROUP_ANIBOOSTER,
	SMART_IO_RL_CGROUP_BACKGROUND,
	SMART_IO_RL_CGROUP_BACKGROUND_L,
	SMART_IO_RL_CGROUP_CAMERA_DAEMON,
	SMART_IO_RL_CGROUP_DEX2OAT,
	SMART_IO_RL_CGROUP_FOREGROUND,
	SMART_IO_RL_CGROUP_FOREGROUND_L,
	SMART_IO_RL_CGROUP_FOREGROUND_WINDOW,
	SMART_IO_RL_CGROUP_RESTRICTED,
	SMART_IO_RL_CGROUP_SYSTEM_BACKGROUND,
	SMART_IO_RL_CGROUP_SYSTEM_BACKGROUND_H,
	SMART_IO_RL_CGROUP_SYSTEM_BACKGROUND_L,
	SMART_IO_RL_CGROUP_TOP_APP,
};

enum smart_io_rl_sector_class {
	SMART_IO_RL_SECTOR_UNKNOWN = 0,
	SMART_IO_RL_SECTOR_SEQ_FORWARD,
	SMART_IO_RL_SECTOR_SEQ_BACKWARD,
	SMART_IO_RL_SECTOR_NON_CONTIGUOUS,
};

struct smart_io_rl_state {
	u32 size_bytes;
	u8 sector_class;
	u8 file_ext;
	u8 uid_class;
	u8 cpu_cgroup_class;
	u8 thread_role;
	u8 is_front;
	u8 is_loop;
	u8 is_meta;
	u8 is_rahead;
	u8 is_sync;
	u8 is_prio;
	u8 is_fua;
	u8 is_preflush;
	u8 is_pgf;
	u8 is_pi;
	u8 original_ioprio_class;
	struct smart_io_rl_device_snapshot device;
};

/* Fallback / anomaly reasons (doc §11). */
enum smart_io_rl_fallback {
	SMART_IO_RL_FB_NONE = 0,
	SMART_IO_RL_FB_STATE_MISSING = 1,
	SMART_IO_RL_FB_INVALID_ACTION = 2,
	SMART_IO_RL_FB_ENCODE_ERROR = 3,
};

struct smart_io_rl_stats {
	u64 rl_decision_count;
	u64 rl_protected_count;
	u64 rl_fallback_count;
	u64 rl_decode_error_count;
	u64 rl_action_vip;
	u64 rl_action_hp;
	u64 rl_action_rt;
	u32 rl_mode;
	u32 rl_fixed_action;
	u32 rl_random_vip_pct;
	u32 rl_random_hp_pct;
	u32 rl_random_rt_pct;
	u32 rl_random_seed;
};

/*
 * Called from dd_insert_request() while holding the scheduler lock, before
 * queue selection. Decides the READ action (VIP/HP/RT), writes the RL hint
 * bits and the final ioprio class, and applies the protection paths and
 * fallback rules. Returns true when the request was RL-controlled.
 * Non-READ and protected requests return false and are left on the fixed path.
 */
bool smart_io_rl_classify_request(struct request *rq);
int smart_io_rl_model_action(const struct smart_io_rl_state *state,
			     u8 *action);
int smart_io_rl_decode_action(u16 ioprio);
u8 smart_io_rl_cpu_cgroup_class(void);

/* Aggregated counters for /proc/smart_io_sched/rl_stats. */
void smart_io_rl_get_stats(struct smart_io_rl_stats *stats);

/* Collection knobs (set from procfs; validated before use). */
int  smart_io_rl_set_mode(u32 mode);
u32  smart_io_rl_get_mode(void);
int  smart_io_rl_set_fixed_action(u32 action);
u32  smart_io_rl_get_fixed_action(void);
int  smart_io_rl_set_random_probs(u32 vip_pct, u32 hp_pct, u32 rt_pct);
int  smart_io_rl_set_random_seed(u32 seed);
bool smart_io_rl_vip_action(u16 ioprio);

#endif
