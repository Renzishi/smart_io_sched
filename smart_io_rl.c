// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/blk-mq.h>
#include <linux/cgroup.h>
#include <linux/errno.h>
#include <linux/ioprio.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/user_namespace.h>

#include "block_helper.h"
#include "smart_io_rl.h"
#include "smart_io_types.h"
#include "smart_io_throttle.h"
#include "system_context.h"
#include "tool.h"
#include "tp_pi.h"

static atomic_t rl_mode = ATOMIC_INIT(SMART_IO_RL_MODE_OFF);
static atomic_t rl_fixed_action = ATOMIC_INIT(SMART_IO_RL_ACTION_HP);
static atomic_t rl_random_vip_pct = ATOMIC_INIT(0);
static atomic_t rl_random_hp_pct = ATOMIC_INIT(0);
static atomic_t rl_random_rt_pct = ATOMIC_INIT(0);
static atomic_t rl_random_seed = ATOMIC_INIT(0);

static atomic_t rl_decision_count = ATOMIC_INIT(0);
static atomic_t rl_protected_count = ATOMIC_INIT(0);
static atomic_t rl_fallback_count = ATOMIC_INIT(0);
static atomic_t rl_decode_error_count = ATOMIC_INIT(0);
static atomic_t rl_action_vip = ATOMIC_INIT(0);
static atomic_t rl_action_hp = ATOMIC_INIT(0);
static atomic_t rl_action_rt = ATOMIC_INIT(0);

/* Per-CPU LCG state for the random collection scheme (race-free in spinlock
 * context; seeded from procfs). */
static DEFINE_PER_CPU(u32, rl_rng_state);

static inline u32 smart_io_rl_lcg_next(void)
{
	u32 *st = this_cpu_ptr(&rl_rng_state);

	*st = *st * 1664525U + 1013904223U;
	return *st;
}

static u8 smart_io_rl_uid_class(uid_t uid)
{
	if (uid == 0)
		return SMART_IO_RL_UID_ROOT;
	if (uid >= 1000 && uid < 2000)
		return SMART_IO_RL_UID_SYSTEM;
	if (uid >= 2000 && uid < 3000)
		return SMART_IO_RL_UID_VENDOR;
	if (uid >= 3000 && uid < 10000)
		return SMART_IO_RL_UID_SYSTEM_APP;
	if (uid >= 10000 && uid < 20000)
		return SMART_IO_RL_UID_PREINSTALLED_APP;
	if (uid >= 20000 && uid < 99000)
		return SMART_IO_RL_UID_NORMAL_APP;
	if (uid >= 99000 && uid < 100000)
		return SMART_IO_RL_UID_ISOLATED;
	return SMART_IO_RL_UID_UNKNOWN;
}

u8 smart_io_rl_cpu_cgroup_class(void)
{
	struct cgroup_subsys_state *css;
	const char *name;
	u8 result = SMART_IO_RL_CGROUP_UNKNOWN;

	name = NULL;
	rcu_read_lock();
#ifdef CONFIG_CGROUP_SCHED
	css = task_css(current, cpu_cgrp_id);
	if (css && css->cgroup && css->cgroup->kn)
		name = READ_ONCE(css->cgroup->kn->name);
	if (name) {
		if (!strcmp(name, "anibooster"))
			result = SMART_IO_RL_CGROUP_ANIBOOSTER;
		else if (!strcmp(name, "background"))
			result = SMART_IO_RL_CGROUP_BACKGROUND;
		else if (!strcmp(name, "background-l"))
			result = SMART_IO_RL_CGROUP_BACKGROUND_L;
		else if (!strcmp(name, "camera-daemon"))
			result = SMART_IO_RL_CGROUP_CAMERA_DAEMON;
		else if (!strcmp(name, "dex2oat"))
			result = SMART_IO_RL_CGROUP_DEX2OAT;
		else if (!strcmp(name, "foreground"))
			result = SMART_IO_RL_CGROUP_FOREGROUND;
		else if (!strcmp(name, "foreground-l"))
			result = SMART_IO_RL_CGROUP_FOREGROUND_L;
		else if (!strcmp(name, "foreground_window"))
			result = SMART_IO_RL_CGROUP_FOREGROUND_WINDOW;
		else if (!strcmp(name, "restricted"))
			result = SMART_IO_RL_CGROUP_RESTRICTED;
		else if (!strcmp(name, "system-background"))
			result = SMART_IO_RL_CGROUP_SYSTEM_BACKGROUND;
		else if (!strcmp(name, "system-background-h"))
			result = SMART_IO_RL_CGROUP_SYSTEM_BACKGROUND_H;
		else if (!strcmp(name, "system-background-l"))
			result = SMART_IO_RL_CGROUP_SYSTEM_BACKGROUND_L;
		else if (!strcmp(name, "top-app"))
			result = SMART_IO_RL_CGROUP_TOP_APP;
	}
#endif
	rcu_read_unlock();
	return result;
}

static bool smart_io_rl_build_state(struct request *rq,
				    struct smart_io_rl_state *state)
{
	uid_t uid;
	int fg_uid;
	char comm[TASK_COMM_LEN];
	char dev_name[SMART_IO_DEV_NAME_LEN] = "unknown";
	struct bio *bio;

	if (!rq || !state)
		return false;

	memset(state, 0, sizeof(*state));
	uid = from_kuid(&init_user_ns, current_uid());
	fg_uid = smart_io_get_fg_uid();
	state->size_bytes = min_t(u64, blk_rq_bytes(rq), U32_MAX);
	state->uid_class = smart_io_rl_uid_class(uid);
	state->is_front = fg_uid >= 0 && (u32)fg_uid == uid;
	state->is_meta = !!(rq->cmd_flags & REQ_META);
	state->is_rahead = !!(rq->cmd_flags & REQ_RAHEAD);
	state->is_sync = !!(rq->cmd_flags & REQ_SYNC);
	state->is_prio = !!(rq->cmd_flags & REQ_PRIO);
	state->is_fua = !!(rq->cmd_flags & REQ_FUA);
	state->is_preflush = !!(rq->cmd_flags & REQ_PREFLUSH);
	state->original_ioprio_class = IOPRIO_PRIO_CLASS(req_get_ioprio(rq));
	state->sector_class = smart_io_throttle_get_rl_sector_class(rq);
	state->cpu_cgroup_class = smart_io_rl_cpu_cgroup_class();
	get_task_comm(comm, current);
	state->thread_role = match_thread_role(uid, current->pid, current->tgid,
					       comm, fg_uid);
	bio = rq->bio;
	if (bio)
		state->is_sync |= !!(bio->bi_opf & REQ_SYNC);
	state->file_ext = extract_first_bio_file_ext(bio);
	extract_device_name(rq, dev_name, sizeof(dev_name));
	state->is_loop = !strncmp(dev_name, "loop", 4);
	state->is_pgf = !!(IOPRIO_PRIO_HINT(req_get_ioprio(rq)) &
				   SMART_IO_PGF_IOPRIO_HINT);
	state->is_pi = !!(IOPRIO_PRIO_HINT(req_get_ioprio(rq)) &
				  SMART_IO_PI_IOPRIO_HINT);
	return smart_io_throttle_get_rl_device_snapshot(rq, &state->device);
}

/*
 * Encode an RL action into the request ioprio (doc §3):
 *   VIP: keep class, set HP + VIP route bits + RL_ACTION_VIP
 *   HP : keep class, set HP route bit        + RL_ACTION_HP
 *   RT : set class RT, clear RL-controllable route bits (HP/VIP, keep PGF/PI)
 *         + RL_ACTION_RT
 * Old RL action bits are cleared first so the write is idempotent across
 * requeues. PGF/PI/MAIN source bits are never cleared by an RL write.
 */
static u16 smart_io_rl_encode(u16 ioprio, u8 action)
{
	u8 ioprio_class = IOPRIO_PRIO_CLASS(ioprio);
	u16 old_hint = IOPRIO_PRIO_HINT(ioprio);
	u8 level = IOPRIO_PRIO_LEVEL(ioprio);
	u16 new_hint;
	u8 new_class;

	new_hint = old_hint & ~(SMART_IO_RL_ACTION_MASK |
				       SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT);
	new_class = ioprio_class;
	if (new_class == IOPRIO_CLASS_NONE) {
		new_class = IOPRIO_CLASS_BE;
		level = IOPRIO_BE_NORM;
	}

	if (action == SMART_IO_RL_ACTION_RT) {
		new_class = IOPRIO_CLASS_RT;
		new_hint &= ~(SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT);
		new_hint |= (u16)SMART_IO_RL_ACTION_RT << SMART_IO_RL_ACTION_SHIFT;
	} else if (action == SMART_IO_RL_ACTION_VIP) {
		new_hint |= SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT;
		new_hint |= (u16)SMART_IO_RL_ACTION_VIP << SMART_IO_RL_ACTION_SHIFT;
	} else { /* SMART_IO_RL_ACTION_HP */
		new_hint |= SMART_IO_HP_IOPRIO_HINT;
		new_hint |= (u16)SMART_IO_RL_ACTION_HP << SMART_IO_RL_ACTION_SHIFT;
	}

	return IOPRIO_PRIO_VALUE_HINT(new_class, level, new_hint);
}

/*
 * Deterministic BC rule (assumption pending doc freeze, recorded in the run
 * manifest): foreground UX/front reads -> VIP, background/kernel/fs reads ->
 * RT, everything else -> HP. Only used in SMART_IO_RL_MODE_RULE.
 */
static u8 smart_io_rl_rule_action(const struct smart_io_rl_state *state)
{
	if (state->is_front && (state->thread_role == ROLE_UX ||
					state->thread_role == ROLE_FRONT))
		return SMART_IO_RL_ACTION_VIP;
	if (state->thread_role == ROLE_BACK || state->thread_role == ROLE_KWORKER ||
	    state->thread_role == ROLE_FS_BACK)
		return SMART_IO_RL_ACTION_RT;
	return SMART_IO_RL_ACTION_HP;
}

int smart_io_rl_model_action(const struct smart_io_rl_state *state, u8 *action)
{
	/* Request-RL weights are not yet deployed in the kernel. Keep a real
	 * entry point so the mode and fallback contract are fixed. */
	if (!state || !action)
		return -EINVAL;
	return -EOPNOTSUPP;
}

int smart_io_rl_decode_action(u16 ioprio)
{
	u16 hint = IOPRIO_PRIO_HINT(ioprio);
	u8 action = (hint & SMART_IO_RL_ACTION_MASK) >>
		SMART_IO_RL_ACTION_SHIFT;
	/* PGF/PI are protection bits and are intentionally ignored here. */
	u16 route = hint & (SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT);

	if (!action)
		return SMART_IO_RL_ACTION_NONE;
	if (action == SMART_IO_RL_ACTION_VIP)
		return route == (SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT) ?
			SMART_IO_RL_ACTION_VIP : -EINVAL;
	if (action == SMART_IO_RL_ACTION_HP)
		return route == SMART_IO_HP_IOPRIO_HINT ? SMART_IO_RL_ACTION_HP : -EINVAL;
	if (action == SMART_IO_RL_ACTION_RT)
		return IOPRIO_PRIO_CLASS(ioprio) == IOPRIO_CLASS_RT &&
			!(route & (SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT)) ?
			SMART_IO_RL_ACTION_RT : -EINVAL;
	return -EINVAL;
}

/*
 * Random exploration within the legal action set (doc §9.C). For META/RAHEAD
 * reads VIP is not legal, so it is excluded and the remaining probabilities
 * are used as-is (summed over HP/RT). O(1), per-CPU LCG, no allocation.
 */
static u8 smart_io_rl_random_action(void)
{
	u32 vip_pct = (u32)atomic_read(&rl_random_vip_pct);
	u32 hp_pct = (u32)atomic_read(&rl_random_hp_pct);
	u32 rt_pct = (u32)atomic_read(&rl_random_rt_pct);
	u32 total, roll;

	total = vip_pct + hp_pct + rt_pct;
	if (!total)
		return SMART_IO_RL_ACTION_HP;

	roll = smart_io_rl_lcg_next() % total;
	if (roll < vip_pct)
		return SMART_IO_RL_ACTION_VIP;
	if (roll < vip_pct + hp_pct)
		return SMART_IO_RL_ACTION_HP;
	return SMART_IO_RL_ACTION_RT;
}

bool smart_io_rl_classify_request(struct request *rq)
{
	u32 mode;
	u16 ioprio;
	u16 hint;
	struct smart_io_rl_state state;
	u8 action;

	if (!rq)
		return false;

	mode = (u32)atomic_read(&rl_mode);
	if (mode == SMART_IO_RL_MODE_OFF)
		return false;

	/* Only READ requests enter the RL path. */
	if (req_op(rq) != REQ_OP_READ)
		return false;

	/* Protection: FUA / PREFLUSH reads keep the native path. */
	if (rq->cmd_flags & (REQ_FUA | REQ_PREFLUSH)) {
		atomic_inc(&rl_protected_count);
		return false;
	}

	ioprio = req_get_ioprio(rq);
	hint = IOPRIO_PRIO_HINT(ioprio);
	/* Requeues must retain the action selected at first insertion. */
	if (hint & SMART_IO_RL_ACTION_MASK)
		return false;

	if (!smart_io_rl_build_state(rq, &state)) {
		atomic_inc(&rl_fallback_count);
		return false;
	}

	if (mode == SMART_IO_RL_MODE_RULE)
		action = smart_io_rl_rule_action(&state);
	else if (mode == SMART_IO_RL_MODE_FIXED)
		action = (u8)atomic_read(&rl_fixed_action);
	else if (mode == SMART_IO_RL_MODE_RANDOM)
		action = smart_io_rl_random_action();
	else {
		int ret = smart_io_rl_model_action(&state, &action);

		if (ret) {
			atomic_inc(&rl_fallback_count);
			return false;
		}
	}

	/* Invalid action (e.g. bad fixed-action knob) falls back to fixed path. */
	if (action != SMART_IO_RL_ACTION_VIP &&
	    action != SMART_IO_RL_ACTION_HP &&
	    action != SMART_IO_RL_ACTION_RT) {
		atomic_inc(&rl_fallback_count);
		atomic_inc(&rl_decode_error_count);
		return false;
	}

	rq->ioprio = smart_io_rl_encode(ioprio, action);

	atomic_inc(&rl_decision_count);
	switch (action) {
	case SMART_IO_RL_ACTION_VIP:
		atomic_inc(&rl_action_vip);
		break;
	case SMART_IO_RL_ACTION_HP:
		atomic_inc(&rl_action_hp);
		break;
	case SMART_IO_RL_ACTION_RT:
		atomic_inc(&rl_action_rt);
		break;
	}
	return true;
}

void smart_io_rl_get_stats(struct smart_io_rl_stats *stats)
{
	if (!stats)
		return;
	stats->rl_decision_count = (u64)atomic_read(&rl_decision_count);
	stats->rl_protected_count = (u64)atomic_read(&rl_protected_count);
	stats->rl_fallback_count = (u64)atomic_read(&rl_fallback_count);
	stats->rl_decode_error_count = (u64)atomic_read(&rl_decode_error_count);
	stats->rl_action_vip = (u64)atomic_read(&rl_action_vip);
	stats->rl_action_hp = (u64)atomic_read(&rl_action_hp);
	stats->rl_action_rt = (u64)atomic_read(&rl_action_rt);
	stats->rl_mode = (u32)atomic_read(&rl_mode);
	stats->rl_fixed_action = (u32)atomic_read(&rl_fixed_action);
	stats->rl_random_vip_pct = (u32)atomic_read(&rl_random_vip_pct);
	stats->rl_random_hp_pct = (u32)atomic_read(&rl_random_hp_pct);
	stats->rl_random_rt_pct = (u32)atomic_read(&rl_random_rt_pct);
	stats->rl_random_seed = (u32)atomic_read(&rl_random_seed);
}

int smart_io_rl_set_mode(u32 mode)
{
	if (mode > SMART_IO_RL_MODE_MAX)
		return -EINVAL;
	atomic_set(&rl_mode, mode);
	return 0;
}

u32 smart_io_rl_get_mode(void)
{
	return (u32)atomic_read(&rl_mode);
}

int smart_io_rl_set_fixed_action(u32 action)
{
	if (action != SMART_IO_RL_ACTION_VIP &&
	    action != SMART_IO_RL_ACTION_HP &&
	    action != SMART_IO_RL_ACTION_RT)
		return -EINVAL;
	atomic_set(&rl_fixed_action, action);
	return 0;
}

u32 smart_io_rl_get_fixed_action(void)
{
	return (u32)atomic_read(&rl_fixed_action);
}

int smart_io_rl_set_random_probs(u32 vip_pct, u32 hp_pct, u32 rt_pct)
{
	if (vip_pct > 100U || hp_pct > 100U || rt_pct > 100U ||
		vip_pct + hp_pct + rt_pct != 100U)
		return -EINVAL;
	atomic_set(&rl_random_vip_pct, vip_pct);
	atomic_set(&rl_random_hp_pct, hp_pct);
	atomic_set(&rl_random_rt_pct, rt_pct);
	return 0;
}

int smart_io_rl_set_random_seed(u32 seed)
{
	unsigned int cpu;

	atomic_set(&rl_random_seed, seed);
	for_each_possible_cpu(cpu)
		per_cpu(rl_rng_state, cpu) = seed ^ (0x9e3779b9U * (cpu + 1));
	return 0;
}
