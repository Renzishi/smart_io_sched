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
#include "smart_io_rl_policy.h"
#include "smart_io_throttle.h"
#include "smart_io_types.h"
#include "system_context.h"
#include "trace_instance.h"
#include "tool.h"

static atomic_t rl_mode = ATOMIC_INIT(SMART_IO_RL_MODE_OFF);
static atomic_t rl_elevate_pct = ATOMIC_INIT(0);
static atomic_t rl_random_seed = ATOMIC_INIT(0);
static atomic_t rl_trace = ATOMIC_INIT(0);
static atomic_t rl_decision_id = ATOMIC_INIT(0);

static atomic_t rl_decision_count = ATOMIC_INIT(0);
static atomic_t rl_keep_count = ATOMIC_INIT(0);
static atomic_t rl_elevate_count = ATOMIC_INIT(0);
static atomic_t rl_eligible_count = ATOMIC_INIT(0);
static atomic_t rl_protected_count = ATOMIC_INIT(0);
static atomic_t rl_native_vip_count = ATOMIC_INIT(0);
static atomic_t rl_fallback_count = ATOMIC_INIT(0);
static atomic_t rl_decode_error_count = ATOMIC_INIT(0);
/* S-gate (S-only random promotion): when set, RANDOM mode only rolls
 * KEEP/ELEVATE while the main thread is in an S block interval; otherwise it
 * forces KEEP. Default 0 = current behavior (no gate). */
static atomic_t rl_s_gate = ATOMIC_INIT(0);
/* Decisions made while the main thread was in an S block interval (S-gate
 * verification: expected == rl_decision_count when the gate is active). */
static atomic_t rl_main_in_s_count = ATOMIC_INIT(0);

/* Per-CPU LCG streams for random collection (race-free in spinlock context).
 * Each CPU derives an independent stream from the configured seed so
 * concurrent CPUs do not produce the same sequence (doc §B1.6). */
static DEFINE_PER_CPU(u32, rl_rng_state);
static DEFINE_PER_CPU(u64, rl_stream_seq);

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

/*
 * Baseline queue selection for a READ before any RL action, mirroring the
 * non-RL branch of smart_io_throttle_select_queue (hint-driven, no RL bits).
 */
static u8 smart_io_rl_base_queue_class(u16 ioprio)
{
	u16 hint = IOPRIO_PRIO_HINT(ioprio);

	if (hint & (SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT))
		return SMART_IO_QUEUE_HP;
	return IOPRIO_PRIO_CLASS(ioprio) == IOPRIO_CLASS_RT ?
		SMART_IO_QUEUE_RT : SMART_IO_QUEUE_BE;
}

static u8 smart_io_rl_source_class(bool is_front, u8 cgroup_class, int fg_uid)
{
	bool top_app = cgroup_class == SMART_IO_RL_CGROUP_TOP_APP;

	if (fg_uid < 0)
		return SMART_IO_RL_SRC_UNKNOWN;
	if (is_front && top_app)
		return SMART_IO_RL_SRC_FG_TOPAPP;
	if (is_front)
		return SMART_IO_RL_SRC_FG_ONLY;
	if (top_app)
		return SMART_IO_RL_SRC_TOPAPP_ONLY;
	return SMART_IO_RL_SRC_NEITHER;
}

/*
 * Main-thread (foreground app main thread) run-state observation for State v2
 * (doc §B2/§B3). The block (S/D) interval start is recorded from the
 * sched_switch/sched_wakeup callbacks (smart_io_rl_track_*), so the online
 * State keeps updating whether or not raw printing is enabled. main_state()
 * reads the current task state on demand and reports the age of the current
 * S/D interval. Snapshot semantics only: not cross-subsystem atomic.
 */
static u64 main_block_start_ns;
static bool main_in_block;
/* 0 = no in-flight block; otherwise the state the main thread switched out
 * into (TASK_INTERRUPTIBLE=3 S / TASK_UNINTERRUPTIBLE=4 D). */
static u8 main_block_state;

void smart_io_rl_track_switch_out(unsigned long prev_state, bool preempt)
{
	/* A preempted-out main thread is running again after reschedule; a
	 * switch-out into S/D starts a block interval. */
	if (preempt)
		return;
	if (prev_state == TASK_INTERRUPTIBLE || prev_state == TASK_UNINTERRUPTIBLE) {
		WRITE_ONCE(main_in_block, true);
		WRITE_ONCE(main_block_start_ns, ktime_get_boottime_ns());
		WRITE_ONCE(main_block_state, (u8)prev_state);
	}
}

void smart_io_rl_track_switch_in(void)
{
	/* Scheduled back: any in-flight block interval ends. */
	WRITE_ONCE(main_in_block, false);
	WRITE_ONCE(main_block_state, 0);
}

void smart_io_rl_track_wakeup(void)
{
	/* A wakeup makes the thread runnable: the S/D block interval ends. */
	WRITE_ONCE(main_in_block, false);
	WRITE_ONCE(main_block_state, 0);
}

void smart_io_rl_note_fg_change(pid_t fg_main_pid, u32 fg_uid)
{
	(void)fg_main_pid;
	(void)fg_uid;
	/* Foreground epoch change: reset the tracked main-thread block-interval
	 * state so the new epoch starts clean. The effective identity is read
	 * fresh from fg_main_pid / fg_uid at each observation. */
	WRITE_ONCE(main_in_block, false);
	WRITE_ONCE(main_block_start_ns, 0);
	WRITE_ONCE(main_block_state, 0);
}

/* True iff the main thread is in an in-flight S (TASK_INTERRUPTIBLE) block
 * interval. Reads the event-driven tracker (no per-call task lookup). */
bool smart_io_rl_main_in_s(void)
{
	return READ_ONCE(main_in_block) &&
		READ_ONCE(main_block_state) == TASK_INTERRUPTIBLE;
}

int smart_io_rl_main_state(u64 *block_age_us)
{
	int fg_uid;
	u64 now = 0;
	u64 age = 0;
	u8 state = SMART_IO_RL_MAIN_UNKNOWN;

	*block_age_us = 0;
	fg_uid = smart_io_get_fg_uid();
	if (fg_uid < 0)
		return SMART_IO_RL_MAIN_UNKNOWN;

	/*
	 * Read the event-driven tracker instead of snapshotting fg_main_pid:
	 * the main thread's S/D state is recorded at the sched_switch/sched_wakeup
	 * event (see fg_main_thread_matches + smart_io_rl_track_*), so this is
	 * correct even while fg_main_pid is stale (0 or the previous app's pid).
	 * Not in an S/D block interval means the main thread is on-CPU (running).
	 */
	if (READ_ONCE(main_in_block)) {
		if (READ_ONCE(main_block_state) == TASK_INTERRUPTIBLE)
			state = SMART_IO_RL_MAIN_SLEEPING;
		else if (READ_ONCE(main_block_state) == TASK_UNINTERRUPTIBLE)
			state = SMART_IO_RL_MAIN_BLOCKED;
		now = ktime_get_boottime_ns();
		age = now - READ_ONCE(main_block_start_ns);
	} else {
		state = SMART_IO_RL_MAIN_RUNNING;
	}
	if (age > U32_MAX)
		age = U32_MAX;
	*block_age_us = age;
	return state;
}

static bool smart_io_rl_build_state(struct request *rq,
				    struct smart_io_rl_state *state)
{
	uid_t uid;
	int fg_uid;
	char comm[TASK_COMM_LEN];
	char dev_name[SMART_IO_DEV_NAME_LEN] = "unknown";
	u16 ioprio;
	u16 hint;
	u64 age = 0;
	u8 cgroup_class;
	struct bio *bio;

	if (!rq || !state)
		return false;

	memset(state, 0, sizeof(*state));
	ioprio = req_get_ioprio(rq);
	hint = IOPRIO_PRIO_HINT(ioprio);
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
	state->original_ioprio_class = IOPRIO_PRIO_CLASS(ioprio);
	state->base_queue = smart_io_rl_base_queue_class(ioprio);
	state->base_is_vip = !!(hint & SMART_IO_VIP_IOPRIO_HINT);
	state->base_hp_hint = !!(hint & SMART_IO_HP_IOPRIO_HINT);
	cgroup_class = smart_io_rl_cpu_cgroup_class();
	state->cpu_cgroup_class = cgroup_class;
	state->source_class = smart_io_rl_source_class(state->is_front,
						       cgroup_class, fg_uid);
	state->main_state = (u8)smart_io_rl_main_state(&age);
	state->main_block_age_us = (u32)age;
	state->sector_class = smart_io_throttle_get_rl_sector_class(rq);
	get_task_comm(comm, current);
	state->thread_role = match_thread_role(uid, current->pid, current->tgid,
					       comm, fg_uid);
	bio = rq->bio;
	if (bio)
		state->is_sync |= !!(bio->bi_opf & REQ_SYNC);
	state->file_ext = extract_first_bio_file_ext(bio);
	extract_device_name(rq, dev_name, sizeof(dev_name));
	state->is_loop = !strncmp(dev_name, "loop", 4);
	state->is_pgf = !!(hint & SMART_IO_PGF_IOPRIO_HINT);
	state->is_pi = !!(hint & SMART_IO_PI_IOPRIO_HINT);
	/* Layer pressure for State v2 (doc §B3). */
	smart_io_throttle_get_rl_pressure_snapshot(rq, state);
	return smart_io_throttle_get_rl_device_snapshot(rq, &state->device);
}

/*
 * Encode a binary RL decision into the request ioprio (doc §B1):
 *   KEEP    : keep the original ioprio (class + hint) exactly, so the
 *             request is routed to the same baseline queue with the same
 *             VIP treatment; only the RL decision bit is recorded.
 *   ELEVATE : set the HP + VIP route bits (real promotion into the HP
 *             queue VIP segment) and record the RL decision bit.
 * PGF/PI/MAIN source bits are never cleared.
 */
static u16 smart_io_rl_encode(u16 ioprio, u8 action)
{
	u8 ioprio_class = IOPRIO_PRIO_CLASS(ioprio);
	u16 hint = IOPRIO_PRIO_HINT(ioprio);
	u8 level = IOPRIO_PRIO_LEVEL(ioprio);

	if (action == SMART_IO_RL_ACTION_KEEP) {
		hint &= ~SMART_IO_RL_ACTION_MASK;
		hint |= (u16)SMART_IO_RL_ACTION_KEEP << SMART_IO_RL_ACTION_SHIFT;
		return IOPRIO_PRIO_VALUE_HINT(ioprio_class, level, hint);
	}

	/* ELEVATE */
	if (ioprio_class == IOPRIO_CLASS_NONE) {
		ioprio_class = IOPRIO_CLASS_BE;
		level = IOPRIO_BE_NORM;
	}
	hint &= ~SMART_IO_RL_ACTION_MASK;
	hint |= (u16)SMART_IO_RL_ACTION_ELEVATE << SMART_IO_RL_ACTION_SHIFT;
	hint |= SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT;
	return IOPRIO_PRIO_VALUE_HINT(ioprio_class, level, hint);
}

int smart_io_rl_model_action(const struct smart_io_rl_state *state, u8 *action)
{
	int fg_uid;

	/* MODEL mode (doc §Phase 3): the learned policy is deployed as an
	 * offline-quantized decision table (smart_io_rl_policy.c), not as
	 * network weights. The table lookup is O(1) integer math; when no
	 * table is loaded / enabled / matched, it returns -EOPNOTSUPP and the
	 * caller falls back to KEEP (safe baseline path). */
	if (!state || !action)
		return -EINVAL;
	fg_uid = smart_io_get_fg_uid();
	return smart_io_rl_policy_decide(state, (u32)(fg_uid >= 0 ? fg_uid : 0),
					 action);
}

int smart_io_rl_decode_action(u16 ioprio)
{
	u16 hint = IOPRIO_PRIO_HINT(ioprio);
	u8 action = (hint & SMART_IO_RL_ACTION_MASK) >>
		SMART_IO_RL_ACTION_SHIFT;
	/* PGF/PI are protection bits and are intentionally ignored here. */
	u16 route = hint & (SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT);

	if (action == SMART_IO_RL_ACTION_KEEP)
		return SMART_IO_RL_ACTION_KEEP;
	if (action == SMART_IO_RL_ACTION_ELEVATE)
		return route == (SMART_IO_HP_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT) ?
			SMART_IO_RL_ACTION_ELEVATE : -EINVAL;
	if (!action)
		return SMART_IO_RL_ACTION_NONE;
	return -EINVAL;
}

void smart_io_rl_note_decode_error(void)
{
	atomic_inc(&rl_decode_error_count);
}

static u8 smart_io_rl_random_action(u32 cpu, u64 *stream_seq_out)
{
	u32 pct = (u32)atomic_read(&rl_elevate_pct);
	u64 seq;
	u32 roll;

	if (pct == 0)
		return SMART_IO_RL_ACTION_KEEP;
	if (pct >= 100)
		return SMART_IO_RL_ACTION_ELEVATE;

	roll = smart_io_rl_lcg_next();
	seq = per_cpu(rl_stream_seq, cpu);
	per_cpu(rl_stream_seq, cpu) = seq + 1;
	*stream_seq_out = seq;

	return roll % 100U < pct ? SMART_IO_RL_ACTION_ELEVATE
				 : SMART_IO_RL_ACTION_KEEP;
}

static void smart_io_rl_trace_decision(const struct request *rq,
				       const struct smart_io_rl_state *state,
				       bool state_valid, u32 mode,
				       bool eligible, u8 prot, u8 fb,
				       u8 action, u8 applied, u32 pct,
				       u32 cpu, u64 stream_seq)
{
	u64 decision_id = atomic_inc_return(&rl_decision_id);
	u8 final_q;
	u8 vip_t;
	u8 sg;

	/* S-gate diagnostic: 1 iff the gate is enabled AND the main thread is
	 * in an S block interval at decision time. Offline verification uses
	 * this to confirm the gate actually triggered (sg=1 rows are S-only). */
	sg = (u8)(atomic_read(&rl_s_gate) && smart_io_rl_main_in_s());

	if (unlikely(!atomic_read(&rawdata_trace_enabled) ||
		     !atomic_read(&rl_trace)))
		return;

	/*
	 * Final queue + VIP treatment (doc §B1.3): ELEVATE physically routes
	 * into the HP queue VIP segment; KEEP and all non-decided (protected /
	 * fallback) requests keep the baseline queue and VIP treatment exactly.
	 */
	final_q = (applied == SMART_IO_RL_ACTION_ELEVATE) ?
		SMART_IO_QUEUE_HP : (state_valid ? state->base_queue : 0);
	vip_t = (applied == SMART_IO_RL_ACTION_ELEVATE) ? 1U :
		(state_valid ? state->base_is_vip : 0U);

	smart_io_raw_emit("rl_decision: ts_ns=%llu decision_id=%llu rq_p=%p cpu=%u stream_seq=%llu mode=%u eligible=%u prot=%u fb=%u selected=%u applied=%u mu_elevate_pct=%u base_q=%u base_vip=%u base_class=%u final_q=%u vip_t=%u size=%u sector=%u uid_class=%u cgroup=%u role=%u front=%u loop=%u meta=%u rahead=%u sync=%u prio=%u fua=%u preflush=%u pgf=%u pi=%u src=%u base_hp=%u main_state=%u main_age_us=%u r_flt=%u w_flt=%u r_wait=%u w_wait=%u tag_used=%u tag_max=%u r_lat=%u w_lat=%u vip_wait=%u vip_flt=%u hp_nwait=%u hp_nflt=%u rt_wait=%u be_wait=%u rl_vip_w=%u rl_vip_f=%u nat_vip_w=%u nat_vip_f=%u nat_vip_wb=%u rl_vip_wb=%u sg=%u\n",
			  ktime_get_boottime_ns(), decision_id, (void *)rq,
			  cpu, stream_seq, mode, eligible ? 1U : 0U, prot, fb,
			  action, applied, pct,
			  state_valid ? state->base_queue : 0,
			  state_valid ? state->base_is_vip : 0,
			  state_valid ? state->original_ioprio_class : 0,
			  final_q, vip_t,
			  state_valid ? state->size_bytes : 0,
			  state_valid ? state->sector_class : 0,
			  state_valid ? state->uid_class : 0,
			  state_valid ? state->cpu_cgroup_class : 0,
			  state_valid ? state->thread_role : 0,
			  state_valid ? state->is_front : 0,
			  state_valid ? state->is_loop : 0,
			  state_valid ? state->is_meta : 0,
			  state_valid ? state->is_rahead : 0,
			  state_valid ? state->is_sync : 0,
			  state_valid ? state->is_prio : 0,
			  state_valid ? state->is_fua : 0,
			  state_valid ? state->is_preflush : 0,
			  state_valid ? state->is_pgf : 0,
			  state_valid ? state->is_pi : 0,
			  state_valid ? state->source_class : 0,
			  state_valid ? state->base_hp_hint : 0,
			  state_valid ? state->main_state : 0,
			  state_valid ? state->main_block_age_us : 0,
			  state_valid ? state->device.r_flt : 0,
			  state_valid ? state->device.w_flt : 0,
			  state_valid ? state->device.r_wait : 0,
			  state_valid ? state->device.w_wait : 0,
			  state_valid ? state->device.tag_used : 0,
			  state_valid ? state->device.tag_max : 0,
			  state_valid ? state->device.r_lat_us : 0,
			  state_valid ? state->device.w_lat_us : 0,
			  state_valid ? state->vip_wait : 0,
			  state_valid ? state->vip_inflight : 0,
			  state_valid ? state->hp_nonvip_wait : 0,
			  state_valid ? state->hp_nonvip_inflight : 0,
			  state_valid ? state->rt_wait : 0,
			  state_valid ? state->be_wait : 0,
			  state_valid ? state->rl_vip_wait : 0,
			  state_valid ? state->rl_vip_inflight : 0,
			  state_valid ? state->native_vip_wait : 0,
			  state_valid ? state->native_vip_inflight : 0,
			  state_valid ? state->native_vip_wait_bytes : 0,
			  state_valid ? state->rl_vip_wait_bytes : 0,
			  sg);
}

bool smart_io_rl_classify_request(struct request *rq)
{
	u32 mode, pct = 0, cpu = 0;
	u16 ioprio, hint;
	u8 prot = SMART_IO_RL_PROT_NONE;
	u8 fb = SMART_IO_RL_FB_NONE;
	u8 action = SMART_IO_RL_ACTION_NONE;
	u8 applied = SMART_IO_RL_ACTION_NONE;
	bool eligible = false;
	bool state_valid = false;
	bool decided = false;
	u64 stream_seq = 0;
	struct smart_io_rl_state state;

	if (!rq)
		return false;

	mode = (u32)atomic_read(&rl_mode);
	if (mode == SMART_IO_RL_MODE_OFF)
		return false;

	/* Only READ requests enter the RL path; WRITE stays on the baseline. */
	if (req_op(rq) != REQ_OP_READ)
		return false;

	ioprio = req_get_ioprio(rq);
	hint = IOPRIO_PRIO_HINT(ioprio);

	/* Protection (doc §A2/§B1.7): protocol requests keep the fixed path;
	 * checked before sampling so protection is never a random KEEP. */
	if (rq->cmd_flags & (REQ_FUA | REQ_PREFLUSH)) {
		prot = (rq->cmd_flags & REQ_FUA) ? SMART_IO_RL_PROT_FUA
						 : SMART_IO_RL_PROT_PREFLUSH;
		goto record;
	}
	if (hint & SMART_IO_PGF_IOPRIO_HINT) {
		prot = SMART_IO_RL_PROT_PGF;
		goto record;
	}
	if (hint & SMART_IO_PI_IOPRIO_HINT) {
		prot = SMART_IO_RL_PROT_PI;
		goto record;
	}
	if (hint & SMART_IO_VIP_IOPRIO_HINT) {
		prot = SMART_IO_RL_PROT_NATIVE_VIP;
		goto record;
	}

	/* Requeue/merge: retain the action selected at first insertion. */
	if (hint & SMART_IO_RL_ACTION_MASK)
		return false;

	eligible = true;
	if (!smart_io_rl_build_state(rq, &state)) {
		fb = SMART_IO_RL_FB_STATE_MISSING;
		goto record;
	}
	state_valid = true;

	pct = (u32)atomic_read(&rl_elevate_pct);
	if (mode == SMART_IO_RL_MODE_RANDOM) {
		bool in_s = false;

		/* S-gate: only roll while the main thread is in an S block
		 * interval; otherwise force KEEP (no LCG roll consumed). */
		if (atomic_read(&rl_s_gate)) {
			in_s = smart_io_rl_main_in_s();
			if (in_s)
				atomic_inc(&rl_main_in_s_count);
			if (!in_s)
				action = SMART_IO_RL_ACTION_KEEP;
		}
		if (action == SMART_IO_RL_ACTION_NONE) {
			cpu = raw_smp_processor_id();
			action = smart_io_rl_random_action(cpu, &stream_seq);
		}
	} else if (mode == SMART_IO_RL_MODE_MODEL) {
		if (smart_io_rl_model_action(&state, &action)) {
			fb = SMART_IO_RL_FB_INVALID_ACTION;
			goto record;
		}
	} else {
		fb = SMART_IO_RL_FB_INVALID_ACTION;
		goto record;
	}

	/* KEEP and ELEVATE are both valid decided actions (doc §B1.2). */
	if (action != SMART_IO_RL_ACTION_KEEP &&
	    action != SMART_IO_RL_ACTION_ELEVATE) {
		fb = SMART_IO_RL_FB_INVALID_ACTION;
		goto record;
	}

	/* Missing state or metadata failure already fell back above; an
	 * encoded action here is the applied action. */
	rq->ioprio = smart_io_rl_encode(ioprio, action);
	applied = action;
	decided = true;

record:
	if (eligible)
		atomic_inc(&rl_eligible_count);
	if (decided) {
		atomic_inc(&rl_decision_count);
		if (applied == SMART_IO_RL_ACTION_KEEP)
			atomic_inc(&rl_keep_count);
		else
			atomic_inc(&rl_elevate_count);
	} else if (eligible) {
		atomic_inc(&rl_fallback_count);
	} else {
		atomic_inc(&rl_protected_count);
		if (prot == SMART_IO_RL_PROT_NATIVE_VIP)
			atomic_inc(&rl_native_vip_count);
	}
	smart_io_rl_trace_decision(rq, &state, state_valid, mode, eligible,
				   prot, fb, action, applied, pct, cpu,
				   stream_seq);
	return decided;
}

void smart_io_rl_get_stats(struct smart_io_rl_stats *stats)
{
	if (!stats)
		return;
	stats->rl_decision_count = (u64)atomic_read(&rl_decision_count);
	stats->rl_keep_count = (u64)atomic_read(&rl_keep_count);
	stats->rl_elevate_count = (u64)atomic_read(&rl_elevate_count);
	stats->rl_eligible_count = (u64)atomic_read(&rl_eligible_count);
	stats->rl_protected_count = (u64)atomic_read(&rl_protected_count);
	stats->rl_native_vip_count = (u64)atomic_read(&rl_native_vip_count);
	stats->rl_fallback_count = (u64)atomic_read(&rl_fallback_count);
	stats->rl_decode_error_count = (u64)atomic_read(&rl_decode_error_count);
	stats->rl_mode = (u32)atomic_read(&rl_mode);
	stats->rl_elevate_pct = (u32)atomic_read(&rl_elevate_pct);
	stats->rl_random_seed = (u32)atomic_read(&rl_random_seed);
	stats->rl_s_gate = (u32)atomic_read(&rl_s_gate);
	stats->rl_main_in_s_count = (u64)atomic_read(&rl_main_in_s_count);
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

int smart_io_rl_set_elevate_pct(u32 pct)
{
	if (pct > 100U)
		return -EINVAL;
	atomic_set(&rl_elevate_pct, pct);
	return 0;
}

u32 smart_io_rl_get_elevate_pct(void)
{
	return (u32)atomic_read(&rl_elevate_pct);
}

int smart_io_rl_set_random_seed(u32 seed)
{
	unsigned int cpu;

	atomic_set(&rl_random_seed, seed);
	for_each_possible_cpu(cpu) {
		per_cpu(rl_rng_state, cpu) =
			seed ^ (0x9e3779b9U * (cpu + 1));
		per_cpu(rl_stream_seq, cpu) = 0;
	}
	return 0;
}

int smart_io_rl_set_s_gate(bool enabled)
{
	atomic_set(&rl_s_gate, enabled ? 1 : 0);
	return 0;
}

bool smart_io_rl_get_s_gate(void)
{
	return atomic_read(&rl_s_gate) != 0;
}

int smart_io_rl_set_trace(bool enabled)
{
	atomic_set(&rl_trace, enabled ? 1 : 0);
	return 0;
}

bool smart_io_rl_trace_enabled(void)
{
	return atomic_read(&rl_trace) != 0;
}

bool smart_io_rl_vip_action(u16 ioprio)
{
	return SMART_IO_RL_ACTION_ELEVATE == smart_io_rl_decode_action(ioprio);
}
