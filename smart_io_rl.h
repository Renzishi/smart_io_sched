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
 * READ request-level RL priority (v2, RL 晋升四目标方案 §B1).
 *
 * Binary action on non-VIP READs:
 *   KEEP    : do not change the baseline routing; the request keeps its
 *             original ioprio (class + hint) so it lands in the same
 *             queue with the same VIP treatment as the prio-test baseline.
 *   ELEVATE : promote the request to VIP treatment in the HP queue by
 *             setting the HP + VIP route hint bits (real promotion, not a
 *             fake physical queue).
 *
 * The decision is encoded in the request ioprio hint field using bits 7-8
 * (SMART_IO_RL_ACTION_*). Those bits are NOT part of SMART_IO_PRIVATE_HINT_MASK
 * (bits 3-6,9); they are parsed offline from the final ioprio recorded by
 * block_rq_insert and drive queue selection (smart_io_throttle_select_rq_queue)
 * and VIP treatment (is_vip_request). The encode/decode/selection/treatment
 * paths must all read these bits consistently.
 *
 * The route/source hint bits (HP/PGF/PI/VIP/MAIN, bits 3-6 and 9) come from
 * smart_io_types.h; this file only adds the RL-action bits (7-8) on top.
 */

#include "smart_io_types.h"

#define SMART_IO_RL_ACTION_SHIFT 7
#define SMART_IO_RL_ACTION_MASK \
	(0x3U << SMART_IO_RL_ACTION_SHIFT)

#define SMART_IO_RL_ACTION_NONE    0U /* no RL decision recorded */
#define SMART_IO_RL_ACTION_KEEP    2U /* keep baseline routing (decided) */
#define SMART_IO_RL_ACTION_ELEVATE 3U /* promote to HP queue VIP segment */

/* Collection modes: explicit binary mode, no implicit mode selection. */
#define SMART_IO_RL_MODE_OFF     0U
#define SMART_IO_RL_MODE_RANDOM  1U /* random KEEP/ELEVATE with prob p */
#define SMART_IO_RL_MODE_MODEL   2U /* model-decided (fallback until deployed) */

#define SMART_IO_RL_MODE_MAX     2U

/*
 * MODEL-path policy ids (doc §Phase 3): when rl_mode == MODEL the
 * classify path calls exactly one of these policy functions. More policies
 * can be appended here; the MODEL path dispatches on the selected id.
 *   S_ELEVATE : the rule extracted from the sg_v3 offline study - ELEVATE
 *               while the fg main thread is in an S block interval, KEEP
 *               otherwise (in-S-only decision space).
 *   TABLE     : the offline-quantized decision table (smart_io_rl_policy.c);
 *               the original MODEL-path behaviour.
 *   APP_RULES : per-uid explicit rules distilled from the v7 7-app IQL
 *               study (smart_io_rl_policy.c): the rule is selected by the
 *               fg uid; in-S-only decision space; unknown uid -> KEEP.
 *   HIGHTAG   : high-tag conditional rule (Arm B of the 2-arm launch RCT):
 *               in-S-only decision space; ELEVATE while the fg main thread
 *               is in an S block interval AND the device tag occupancy
 *               (tag_used/tag_max) is >= 50%, KEEP otherwise.
 *   KEEP      : explicit all-KEEP policy (Arm A of the per-launch interleaved
 *               RCT). Always KEEP. Exists so Arm A runs through the same
 *               MODEL code path as Arm B (only the policy id differs).
 *   ELEVATE_READ : full in-S ELEVATE (Arm B of the per-launch interleaved RCT):
 *               ELEVATE while the fg main thread is in an S block interval,
 *               KEEP otherwise. The RL path only sees READ requests (WRITE
 *               stays baseline), so this elevates all safe in-S READs.
 */
enum smart_io_rl_policy_id {
	SMART_IO_RL_POLICY_S_ELEVATE = 0,
	SMART_IO_RL_POLICY_TABLE = 1,
	SMART_IO_RL_POLICY_APP_RULES = 2,
	SMART_IO_RL_POLICY_HIGHTAG = 3,
	SMART_IO_RL_POLICY_KEEP = 4,
	SMART_IO_RL_POLICY_ELEVATE_READ = 5,
	SMART_IO_RL_POLICY_MAX,
};

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

/*
 * Submission-time source class relative to the foreground identity
 * (fg UID / TOP_APP). Encoded at the first decision from the current
 * thread context; insert context is recorded separately and never
 * presented as the original submitter.
 */
enum smart_io_rl_source_class {
	SMART_IO_RL_SRC_UNKNOWN = 0,
	SMART_IO_RL_SRC_FG_TOPAPP,  /* fg UID and TOP_APP */
	SMART_IO_RL_SRC_FG_ONLY,    /* fg UID only */
	SMART_IO_RL_SRC_TOPAPP_ONLY,/* TOP_APP only */
	SMART_IO_RL_SRC_NEITHER,
};

/* Main-thread run state for State v2 (observed, not a dependency graph). */
enum smart_io_rl_main_state {
	SMART_IO_RL_MAIN_UNKNOWN = 0,
	SMART_IO_RL_MAIN_RUNNING = 1,
	SMART_IO_RL_MAIN_RUNNABLE = 2, /* woken, not yet scheduled */
	SMART_IO_RL_MAIN_SLEEPING = 3, /* TASK_INTERRUPTIBLE (S) */
	SMART_IO_RL_MAIN_BLOCKED = 4,  /* TASK_UNINTERRUPTIBLE (D) */
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
	u8 base_queue;        /* SMART_IO_QUEUE_* before action */
	u8 base_is_vip;       /* VIP treatment before action */
	u8 source_class;      /* enum smart_io_rl_source_class */
	u8 base_hp_hint;      /* HP route bit before action */
	u8 main_state;        /* enum smart_io_rl_main_state */
	u32 main_block_age_us;/* current S/D interval age; 0 if none */
	u32 vip_wait;         /* HP-queue VIP waiting (all) */
	u32 vip_inflight;
	u32 hp_nonvip_wait;   /* HP-queue non-VIP waiting */
	u32 hp_nonvip_inflight;
	u32 rt_wait;          /* RT queue waiting (all UIDs) */
	u32 be_wait;          /* BE queue waiting (all UIDs) */
	u32 rl_vip_wait;      /* VIPs promoted by this policy (waiting) */
	u32 rl_vip_inflight;
	u32 native_vip_wait;  /* baseline-native VIPs (waiting) */
	u32 native_vip_inflight;
	u32 native_vip_wait_bytes;
	u32 rl_vip_wait_bytes;
	struct smart_io_rl_device_snapshot device;
};

/* Fallback / anomaly reasons (doc §B1/§B4). */
enum smart_io_rl_fallback {
	SMART_IO_RL_FB_NONE = 0,
	SMART_IO_RL_FB_STATE_MISSING = 1,
	SMART_IO_RL_FB_INVALID_ACTION = 2,
	SMART_IO_RL_FB_ENCODE_ERROR = 3,
};

/* Protection reasons (recorded, not fallbacks). */
enum smart_io_rl_protection {
	SMART_IO_RL_PROT_NONE = 0,
	SMART_IO_RL_PROT_FUA,
	SMART_IO_RL_PROT_PREFLUSH,
	SMART_IO_RL_PROT_PGF,
	SMART_IO_RL_PROT_PI,
	SMART_IO_RL_PROT_NATIVE_VIP,
	SMART_IO_RL_PROT_NON_READ,
};

struct smart_io_rl_stats {
	u64 rl_decision_count;
	u64 rl_keep_count;
	u64 rl_elevate_count;
	u64 rl_eligible_count;
	u64 rl_protected_count;
	u64 rl_native_vip_count;
	u64 rl_fallback_count;
	u64 rl_decode_error_count;
	u32 rl_mode;
	u32 rl_elevate_pct;
	u32 rl_random_seed;
	u32 rl_s_gate;        /* S-gate: only promote while main thread is in S */
	u64 rl_main_in_s_count; /* decisions made while main thread was in S */
	u32 rl_policy_id;     /* selected MODEL-path policy (enum smart_io_rl_policy_id) */
};

/*
 * Called from dd_insert_request() while holding the scheduler lock, before
 * queue selection. For READ requests in binary collection mode, decides
 * KEEP/ELEVATE (random or model), records the decision in the ioprio RL
 * bits, applies protection paths and fallback rules, updates the main-thread
 * observation, and emits the gated decision trace. Non-READ and protected
 * requests keep the fixed path.
 */
bool smart_io_rl_classify_request(struct request *rq);
int smart_io_rl_model_action(const struct smart_io_rl_state *state,
			     u8 *action);
int smart_io_rl_decode_action(u16 ioprio);
u8 smart_io_rl_cpu_cgroup_class(void);

/* State v2 main-thread observation (doc §B2/§B3). */
int smart_io_rl_main_state(u64 *block_age_us);
/* True iff the main thread is in an S (TASK_INTERRUPTIBLE) block interval,
 * i.e. waiting on binder/futex/sync (not a D-state block). Event-driven via
 * the sched callbacks (independent of fg_main_pid freshness). Gates S-only
 * random promotion (S-gate). */
bool smart_io_rl_main_in_s(void);
/* Event-driven main-thread block-interval tracking (doc §B2), called from
 * the sched_switch / sched_wakeup callbacks. Runs regardless of raw printing. */
void smart_io_rl_track_switch_out(unsigned long prev_state, bool preempt);
void smart_io_rl_track_switch_in(void);
void smart_io_rl_track_wakeup(void);
void smart_io_rl_note_fg_change(pid_t fg_main_pid, u32 fg_uid);

/* Aggregated counters for /proc/smart_io_sched/rl_stats. */
void smart_io_rl_get_stats(struct smart_io_rl_stats *stats);

/* Collection knobs (set from procfs; validated before use). */
int  smart_io_rl_set_mode(u32 mode);
u32  smart_io_rl_get_mode(void);
int  smart_io_rl_set_elevate_pct(u32 pct);
u32  smart_io_rl_get_elevate_pct(void);
int  smart_io_rl_set_random_seed(u32 seed);
/* S-gate (S-only random promotion): when enabled, RANDOM mode only rolls
 * KEEP/ELEVATE while the main thread is in an S block interval; otherwise it
 * forces KEEP. Default off = current behavior. */
int  smart_io_rl_set_s_gate(bool enabled);
bool smart_io_rl_get_s_gate(void);
/* MODEL-path policy selector (enum smart_io_rl_policy_id). */
int  smart_io_rl_set_policy_id(u32 id);
u32  smart_io_rl_get_policy_id(void);
int  smart_io_rl_set_trace(bool enabled);
bool smart_io_rl_trace_enabled(void);
bool smart_io_rl_vip_action(u16 ioprio);
/* Called from the queue selector when an RL-encoded ioprio decodes to an
 * invalid action; counted for the decode-error fallback stat (doc §B1). */
void smart_io_rl_note_decode_error(void);

#endif
