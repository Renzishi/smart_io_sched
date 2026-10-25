// SPDX-License-Identifier: GPL-2.0
/*
 * Offline learned-policy decision table for MODEL mode (doc §Phase 3).
 *
 * The IQL pi-head (3x128 trunk + 2-dim action head) is too large to run in
 * the kernel hot path. Instead the policy is exported offline as a DENSE
 * decision table over a fixed subset of kernel-indexable state dimensions;
 * each cell stores one bit (0=KEEP, 1=ELEVATE). The kernel computes a flat
 * index from the current struct smart_io_rl_state with integer math only and
 * does an O(1) lookup.
 *
 * GKI constraint: the module must NOT open device files (filp_open is not an
 * exported GKI symbol). So the table is STREAMED in from userspace through a
 * procfs write: `cat /data/.../policy.bin > /proc/smart_io_sched/rl_policy_bin`.
 * The first 35 bytes form the header; the remainder fills the table buffer.
 *
 * Binary layout (little-endian, packed) produced by export_policy_table.py:
 *   off 0  u8  version (must be 1)
 *   off 1  u8  n_dims (1..25)
 *   off 2  u8  bases[25]  (base per dim in GROW_ORDER, 2..32; unused = 0)
 *   off 27 u32 table_size (= prod of the first n_dims bases)
 *   off 31 u32 app_uid    (fg uid this table applies to; 0 = any)
 *   off 35 table_size bytes (0=KEEP, 1=ELEVATE)
 *
 * Fallback contract: table not loaded / not enabled / uid mismatch / index
 * out of bounds -> KEEP (the safe baseline path). MODEL mode with no table
 * therefore behaves exactly like the baseline: no promotion, no harm.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include "smart_io_rl.h"
#include "smart_io_rl_policy.h"

#define SMART_IO_POLICY_HDR_LEN \
	(2U + SMART_IO_POLICY_MAX_DIMS + 4U + 4U) /* 35 */

/*
 * Dimension order MUST match export_policy_table.py GROW_ORDER / the
 * "dims" array of policy_index.json. The kernel index is
 *   key = (((b0*base1 + b1)*base2 + b2)... )  (Horner, GROW_ORDER order).
 */
enum smart_io_rl_policy_dim {
	SMART_IO_POL_S_SG = 0,       /* main-thread S-gate bit (smart_io_rl_main_in_s) */
	SMART_IO_POL_MAIN_STATE,
	SMART_IO_POL_BASE_CLASS,
	SMART_IO_POL_BASE_Q,
	SMART_IO_POL_BASE_HP,
	SMART_IO_POL_SECTOR,
	SMART_IO_POL_SRC,
	SMART_IO_POL_ROLE,
	SMART_IO_POL_UID_CLASS,
	SMART_IO_POL_CGROUP,
	SMART_IO_POL_MAIN_AGE_US,
	SMART_IO_POL_SIZE_KB_LOG2,
	SMART_IO_POL_TAG_OCCUPANCY,
	SMART_IO_POL_R_WAIT,
	SMART_IO_POL_W_WAIT,
	SMART_IO_POL_R_FLT,
	SMART_IO_POL_W_FLT,
	SMART_IO_POL_R_LAT,
	SMART_IO_POL_W_LAT,
	SMART_IO_POL_FRONT,
	SMART_IO_POL_LOOP,
	SMART_IO_POL_META,
	SMART_IO_POL_RAHEAD,
	SMART_IO_POL_SYNC,
	SMART_IO_POL_PRIO,
	SMART_IO_POL_NDIMS
};

static struct smart_io_rl_policy rl_policy;

/* Streaming state (process context, procfs write; not on the hot path). */
static u8 pol_hdr[SMART_IO_POLICY_HDR_LEN];
static u32 pol_hdr_len;
static bool pol_hdr_done;
static u8 *pol_table;   /* staged table buffer (vmalloc) */
static u32 pol_table_filled;

static u32 smart_io_pol_log2_u32(u32 x)
{
	if (!x)
		return 0;
	return 31U - (u32)__builtin_clz(x);
}

static u32 smart_io_pol_log2_bucket(u32 x, u32 base)
{
	u32 b = smart_io_pol_log2_u32(x);

	return b >= base ? base - 1U : b;
}

static u32 smart_io_pol_tagocc(u32 used, u32 maxv, u32 base)
{
	u32 b;

	if (!maxv)
		return 0;
	b = (base * used) / maxv;
	return b >= base ? base - 1U : b;
}

static u32 smart_io_pol_raw(u32 x, u32 base)
{
	return x >= base ? base - 1U : x;
}

/* Bucket one dim from the state; returns 0..base-1. */
static u32 smart_io_pol_bucket(const struct smart_io_rl_state *st,
			       u32 dim, u32 base)
{
	switch (dim) {
	case SMART_IO_POL_S_SG:
		/* S-gate bit: 1 iff the fg main thread is in an S block.
		 * Event-driven tracker (two READ_ONCE flags), hot-path safe. */
		return smart_io_pol_raw(smart_io_rl_main_in_s(), base);
	case SMART_IO_POL_MAIN_STATE:
		return smart_io_pol_raw(st->main_state, base);
	case SMART_IO_POL_BASE_CLASS:
		return smart_io_pol_raw(st->original_ioprio_class, base);
	case SMART_IO_POL_BASE_Q:
		return smart_io_pol_raw(st->base_queue, base);
	case SMART_IO_POL_BASE_HP:
		return smart_io_pol_raw(st->base_hp_hint, base);
	case SMART_IO_POL_SECTOR:
		return smart_io_pol_raw(st->sector_class, base);
	case SMART_IO_POL_SRC:
		return smart_io_pol_raw(st->source_class, base);
	case SMART_IO_POL_ROLE:
		return smart_io_pol_raw(st->thread_role, base);
	case SMART_IO_POL_UID_CLASS:
		return smart_io_pol_raw(st->uid_class, base);
	case SMART_IO_POL_CGROUP:
		return smart_io_pol_raw(st->cpu_cgroup_class, base);
	case SMART_IO_POL_MAIN_AGE_US:
		return smart_io_pol_log2_bucket(st->main_block_age_us, base);
	case SMART_IO_POL_SIZE_KB_LOG2:
		return smart_io_pol_log2_bucket(st->size_bytes >> 10, base);
	case SMART_IO_POL_TAG_OCCUPANCY:
		return smart_io_pol_tagocc(st->device.tag_used,
					   st->device.tag_max, base);
	case SMART_IO_POL_R_WAIT:
		return smart_io_pol_log2_bucket(st->device.r_wait, base);
	case SMART_IO_POL_W_WAIT:
		return smart_io_pol_log2_bucket(st->device.w_wait, base);
	case SMART_IO_POL_R_FLT:
		return smart_io_pol_log2_bucket(st->device.r_flt, base);
	case SMART_IO_POL_W_FLT:
		return smart_io_pol_log2_bucket(st->device.w_flt, base);
	case SMART_IO_POL_R_LAT:
		return smart_io_pol_log2_bucket(st->device.r_lat_us, base);
	case SMART_IO_POL_W_LAT:
		return smart_io_pol_log2_bucket(st->device.w_lat_us, base);
	case SMART_IO_POL_FRONT:
		return smart_io_pol_raw(st->is_front, base);
	case SMART_IO_POL_LOOP:
		return smart_io_pol_raw(st->is_loop, base);
	case SMART_IO_POL_META:
		return smart_io_pol_raw(st->is_meta, base);
	case SMART_IO_POL_RAHEAD:
		return smart_io_pol_raw(st->is_rahead, base);
	case SMART_IO_POL_SYNC:
		return smart_io_pol_raw(st->is_sync, base);
	case SMART_IO_POL_PRIO:
		return smart_io_pol_raw(st->is_prio, base);
	default:
		return 0;
	}
}

/*
 * Parse the staged header, validate, and allocate the table buffer.
 * Returns 0 on success (header is consumed, table buffer ready).
 */
static int smart_io_pol_parse_header(void)
{
	u32 table_size, app_uid, i, prod;

	if (pol_hdr[0] != SMART_IO_POLICY_VERSION)
		return -EINVAL;
	rl_policy.n_dims = pol_hdr[1];
	if (rl_policy.n_dims < 1 ||
	    rl_policy.n_dims > SMART_IO_POLICY_MAX_DIMS)
		return -EINVAL;
	for (i = 0; i < SMART_IO_POLICY_MAX_DIMS; i++)
		rl_policy.bases[i] = pol_hdr[2 + i];
	memcpy(&table_size, &pol_hdr[2 + SMART_IO_POLICY_MAX_DIMS], 4);
	memcpy(&app_uid, &pol_hdr[2 + SMART_IO_POLICY_MAX_DIMS + 4], 4);
	if (!table_size || table_size > SMART_IO_POLICY_MAX_SIZE)
		return -EINVAL;
	rl_policy.app_uid = app_uid;
	/* verify table_size == prod of the first n_dims bases */
	prod = 1;
	for (i = 0; i < rl_policy.n_dims; i++) {
		u32 b = rl_policy.bases[i];

		if (b < 2U || b > SMART_IO_POLICY_MAX_BASE)
			return -EINVAL;
		if (prod > table_size / b) /* overflow guard */
			return -EINVAL;
		prod *= b;
	}
	if (prod != table_size)
		return -EINVAL;
	rl_policy.table_size = table_size;
	pol_table = vmalloc(table_size);
	if (!pol_table)
		return -ENOMEM;
	memset(pol_table, 0, table_size);
	pol_table_filled = 0;
	return 0;
}

/*
 * Stage table bytes into the buffer; when complete, publish the table and
 * mark it loaded. Runs in process context (procfs write).
 */
int smart_io_rl_policy_feed(const u8 *data, u32 len)
{
	u32 i;

	if (!data || !len)
		return 0;

	/* Phase 1: accumulate the 34-byte header. */
	if (!pol_hdr_done) {
		u32 need = SMART_IO_POLICY_HDR_LEN - pol_hdr_len;
		u32 take = min_t(u32, len, need);

		memcpy(pol_hdr + pol_hdr_len, data, take);
		pol_hdr_len += take;
		if (pol_hdr_len < SMART_IO_POLICY_HDR_LEN)
			return 0;
		{
			int ret = smart_io_pol_parse_header();

			if (ret)
				return ret;
			pol_hdr_done = true;
		}
		data += take;
		len -= take;
		if (!len)
			return 0;
	}

	/* Phase 2: fill the table buffer. */
	i = 0;
	while (i < len && pol_table_filled < rl_policy.table_size) {
		pol_table[pol_table_filled++] = data[i++];
	}
	/* Any bytes past table_size are ignored (defensive). */

	if (pol_table_filled == rl_policy.table_size) {
		u8 *old = READ_ONCE(rl_policy.table);

		rl_policy.hits = 0;
		rl_policy.misses = 0;
		WRITE_ONCE(rl_policy.table, pol_table);
		rl_policy.loaded = true;
		pol_table = NULL; /* ownership transferred to rl_policy */
		if (old)
			vfree(old);
	}
	return 0;
}

void smart_io_rl_policy_reset(void)
{
	u8 *t;

	pol_hdr_len = 0;
	pol_hdr_done = false;
	if (pol_table) {
		vfree(pol_table);
		pol_table = NULL;
	}
	pol_table_filled = 0;
	t = READ_ONCE(rl_policy.table);
	if (t) {
		WRITE_ONCE(rl_policy.table, NULL);
		rl_policy.loaded = false;
		rl_policy.enabled = false;
		vfree(t);
	}
}

/*
 * Hot-path O(1) decision. Must not sleep, allocate, or take a lock. The
 * table pointer is published only after a full load and freed only at module
 * exit, so a plain READ_ONCE of the pointer is safe against concurrent readers.
 */
int smart_io_rl_policy_decide(const struct smart_io_rl_state *state,
			      u32 fg_uid, u8 *action)
{
	const struct smart_io_rl_policy *p = &rl_policy;
	const u8 *table;
	u64 key = 0;
	u32 i, base;

	table = READ_ONCE(p->table);
	if (unlikely(!table || !READ_ONCE(p->enabled) ||
		     !READ_ONCE(p->loaded))) {
		*action = SMART_IO_RL_ACTION_KEEP;
		return -EOPNOTSUPP;
	}
	if (p->app_uid && p->app_uid != fg_uid) {
		*action = SMART_IO_RL_ACTION_KEEP;
		return -EOPNOTSUPP;
	}
	for (i = 0; i < p->n_dims; i++) {
		base = p->bases[i];
		if (base < 2U)
			return -EOPNOTSUPP;
		key = key * base + smart_io_pol_bucket(state, i, base);
	}
	if (key >= (u64)p->table_size) {
		*action = SMART_IO_RL_ACTION_KEEP;
		return -EOPNOTSUPP;
	}
	*action = table[key] ? SMART_IO_RL_ACTION_ELEVATE
			     : SMART_IO_RL_ACTION_KEEP;
	return 0;
}

/* ----------------------------------------------------------------------
 * Per-app explicit rules (v7): static fg-uid table for the seven trained
 * apps; each entry selects one distilled IQL rule (extract_rules_v7.py,
 * dataset_v7). The decision space is in-S requests only (out-of-S -> KEEP,
 * same contract as S_ELEVATE); an unknown fg uid keeps the safe baseline
 * path (KEEP).
 *
 * Thresholds are expressed in RAW state units (the offline distillation
 * worked on log2-bucketed features; bucket <= b  <=>  raw < 2^(b+1)):
 *   feishu  : ELEVATE iff uid_class <= 1 && w_lat_us <= 7
 *             (s_w_lat bucket <= 2)
 *   douyin  : KEEP iff uid_class <= 1 && rl_vip_inflight >= 2 &&
 *             rl_vip_wait_bytes < 16384 (s_rl_vip_f bucket > 0.5,
 *             s_rl_vip_wb bucket <= 13)
 */
enum smart_io_app_rule_kind {
	SMART_IO_APP_RULE_KEEP = 0,  /* always KEEP (youtube) */
	SMART_IO_APP_RULE_ELEVATE,   /* always ELEVATE while in S */
	SMART_IO_APP_RULE_FEISHU,
	SMART_IO_APP_RULE_DOUYIN,
};

struct smart_io_app_rule {
	u32 uid;
	u8 kind;
};

/* fg uids on device 0826420000013679 (adb pm list packages -U). */
static const struct smart_io_app_rule app_rules[] = {
	{ 10232, SMART_IO_APP_RULE_KEEP },     /* youtube */
	{ 10271, SMART_IO_APP_RULE_FEISHU },   /* feishu */
	{ 10272, SMART_IO_APP_RULE_ELEVATE },  /* toutiao */
	{ 10273, SMART_IO_APP_RULE_KEEP },     /* taobao (v8 retrain: KEEP) */
	{ 10274, SMART_IO_APP_RULE_KEEP },     /* cloudmusic (v8 retrain: KEEP) */
	{ 10275, SMART_IO_APP_RULE_ELEVATE },  /* douyin (v8 retrain: ELEVATE) */
	{ 10276, SMART_IO_APP_RULE_ELEVATE },  /* tiktok_Asia */
};

int smart_io_rl_policy_app_rules(const struct smart_io_rl_state *state,
				 u32 fg_uid, u8 *action)
{
	unsigned int i;
	u8 kind = SMART_IO_APP_RULE_KEEP;

	if (!state || !action)
		return -EINVAL;
	*action = SMART_IO_RL_ACTION_KEEP;
	if (!smart_io_rl_main_in_s())
		return 0;
	for (i = 0; i < ARRAY_SIZE(app_rules); i++) {
		if (app_rules[i].uid == fg_uid) {
			kind = app_rules[i].kind;
			break;
		}
	}
	switch (kind) {
	case SMART_IO_APP_RULE_ELEVATE:
		*action = SMART_IO_RL_ACTION_ELEVATE;
		break;
	case SMART_IO_APP_RULE_FEISHU:
		if (state->uid_class <= 1 && state->device.w_lat_us <= 7)
			*action = SMART_IO_RL_ACTION_ELEVATE;
		break;
	case SMART_IO_APP_RULE_DOUYIN:
		if (!(state->uid_class <= 1 &&
		      state->rl_vip_inflight >= 2 &&
		      state->rl_vip_wait_bytes < 16384))
			*action = SMART_IO_RL_ACTION_ELEVATE;
		break;
	default:
		break;
	}
	return 0;
}

/* High-tag conditional rule (Arm B of the 2-arm launch RCT). Decision space
 * is in-S READ requests only (out-of-S -> KEEP; protected requests are
 * filtered upstream in smart_io_rl_classify). While the fg main thread is in
 * an S block interval, ELEVATE iff the device tag occupancy (tag_used /
 * tag_max) is >= 50%; otherwise KEEP. O(1) integer math, no sleep/alloc/lock.
 */
int smart_io_rl_policy_hightag(const struct smart_io_rl_state *state,
			       u8 *action)
{
	if (!state || !action)
		return -EINVAL;
	*action = SMART_IO_RL_ACTION_KEEP;
	if (!smart_io_rl_main_in_s())
		return 0;
	if (state->device.tag_max > 0 &&
	    (u64)state->device.tag_used * 100U >= (u64)state->device.tag_max * 50U)
		*action = SMART_IO_RL_ACTION_ELEVATE;
	return 0;
}

/* Explicit all-KEEP policy (Arm A of the per-launch interleaved RCT). Always
 * KEEP so Arm A runs through the same MODEL code path as Arm B (only the
 * policy id differs). No sleep/alloc/lock. */
int smart_io_rl_policy_keep(const struct smart_io_rl_state *state, u8 *action)
{
	if (!state || !action)
		return -EINVAL;
	*action = SMART_IO_RL_ACTION_KEEP;
	return 0;
}

/* Full in-S ELEVATE (Arm B of the per-launch interleaved RCT): ELEVATE while
 * the fg main thread is in an S block interval, KEEP otherwise. The RL path
 * only sees READ requests (WRITE stays baseline; protected requests are
 * filtered upstream in smart_io_rl_classify), so this elevates all safe in-S
 * READs with full exposure. No sleep/alloc/lock. */
int smart_io_rl_policy_elevate_read(const struct smart_io_rl_state *state,
				    u8 *action)
{
	if (!state || !action)
		return -EINVAL;
	*action = SMART_IO_RL_ACTION_KEEP;
	if (smart_io_rl_main_in_s())
		*action = SMART_IO_RL_ACTION_ELEVATE;
	return 0;
}

void smart_io_rl_policy_set_enabled(bool on)
{
	WRITE_ONCE(rl_policy.enabled, on);
}

void smart_io_rl_policy_status(u8 *version, u8 *n_dims, u32 *table_size,
			       u32 *app_uid, bool *loaded, bool *enabled,
			       u64 *hits, u64 *misses)
{
	if (version)
		*version = rl_policy.loaded ? SMART_IO_POLICY_VERSION : 0;
	if (n_dims)
		*n_dims = rl_policy.n_dims;
	if (table_size)
		*table_size = rl_policy.table_size;
	if (app_uid)
		*app_uid = rl_policy.app_uid;
	if (loaded)
		*loaded = rl_policy.loaded;
	if (enabled)
		*enabled = rl_policy.enabled;
	if (hits)
		*hits = rl_policy.hits;
	if (misses)
		*misses = rl_policy.misses;
}

void smart_io_rl_policy_free(void)
{
	smart_io_rl_policy_reset();
}
