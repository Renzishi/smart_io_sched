// SPDX-License-Identifier: GPL-2.0
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>

#include <block/blk.h>

#include "smart_io_throttle.h"
#include "system_context.h"
#include "trace_instance.h"

// #define SMART_IO_DEFAULT_DEV_LAT_US 4000U
#define SMART_IO_DEFAULT_DEV_LAT_US 1500U
#define SMART_IO_DEFAULT_LIGHT_PCT 75U
#define SMART_IO_DEFAULT_MEDIUM_PCT 50U
#define SMART_IO_DEFAULT_HEAVY_PCT 25U
#define SMART_IO_DEFAULT_BACKGROUND_DEADLINE_MS 2000U
#define SMART_IO_INFERENCE_TIMEOUT_MS 3U

struct smart_io_throttle_config {
	u32 light_pct;
	u32 medium_pct;
	u32 heavy_pct;
	enum smart_io_action_source source;
	struct smart_io_action fixed_action;
};

static atomic_t throttle_enabled = ATOMIC_INIT(0);
static atomic_t dev_lat_threshold_us = ATOMIC_INIT(SMART_IO_DEFAULT_DEV_LAT_US);
static atomic_t background_deadline_ms =
	ATOMIC_INIT(SMART_IO_DEFAULT_BACKGROUND_DEADLINE_MS);
static DEFINE_SPINLOCK(throttle_config_lock);
static struct smart_io_throttle_config throttle_config = {
	.light_pct = SMART_IO_DEFAULT_LIGHT_PCT,
	.medium_pct = SMART_IO_DEFAULT_MEDIUM_PCT,
	.heavy_pct = SMART_IO_DEFAULT_HEAVY_PCT,
	.source = SMART_IO_ACTION_SOURCE_FIXED,
	.fixed_action = {
		.level = SMART_IO_THROTTLE_MEDIUM,
		.policy = SMART_IO_DISPATCH_BASELINE,
	},
};
static LIST_HEAD(active_throttle_queues);
static DEFINE_MUTEX(active_throttle_queues_lock);

static bool smart_io_throttle_snapshot_state_locked(
		struct smart_io_throttle_ctx *ctx,
		struct smart_io_state_snapshot *state, u64 now_ns, u32 queue_max,
		u32 threshold_us);

static const char * const throttle_level_names[] = {
	[SMART_IO_THROTTLE_NO] = "NO",
	[SMART_IO_THROTTLE_LIGHT] = "LIGHT",
	[SMART_IO_THROTTLE_MEDIUM] = "MEDIUM",
	[SMART_IO_THROTTLE_HEAVY] = "HEAVY",
};

static const char * const dispatch_policy_names[] = {
	[SMART_IO_DISPATCH_BASELINE] = "BASELINE",
	[SMART_IO_DISPATCH_SEQ] = "SEQ",
	[SMART_IO_DISPATCH_SMALL] = "SMALL",
};

static const char * const action_source_names[] = {
	[SMART_IO_ACTION_SOURCE_FIXED] = "fixed",
	[SMART_IO_ACTION_SOURCE_MODEL] = "model",
	[SMART_IO_ACTION_SOURCE_MONOTONIC] = "monotonic",
};

static const char * const gate_reason_names[] = {
	[SMART_IO_GATE_NONE] = "none",
	[SMART_IO_GATE_INFERENCE] = "inference",
	[SMART_IO_GATE_DEPTH] = "depth",
};

static const char * const feedback_state_names[] = {
	[SMART_IO_FEEDBACK_NONE] = "none",
	[SMART_IO_FEEDBACK_WAIT_DISPATCH] = "wait_dispatch",
	[SMART_IO_FEEDBACK_WAIT_ISSUE] = "wait_issue",
	[SMART_IO_FEEDBACK_WAIT_COMPLETE] = "wait_complete",
};

static const char * const selection_names[] = {
	[SMART_IO_SELECTION_SPECIAL] = "special",
	[SMART_IO_SELECTION_FOREGROUND_FIFO] = "foreground_fifo",
	[SMART_IO_SELECTION_SEQ_HIT] = "seq_hit",
	[SMART_IO_SELECTION_SEQ_FALLBACK] = "seq_fallback",
	[SMART_IO_SELECTION_SMALL] = "small",
	[SMART_IO_SELECTION_BACKGROUND_BASELINE] = "background_baseline",
	[SMART_IO_SELECTION_BACKGROUND_DEADLINE] = "background_deadline",
};

static struct smart_io_throttle_ctx *
smart_io_throttle_find_ctx_rcu(struct request_queue *q)
{
	struct smart_io_throttle_ctx *ctx;

	list_for_each_entry_rcu(ctx, &active_throttle_queues, active_node) {
		if (ctx->queue == q)
			return ctx;
	}

	return NULL;
}

static inline struct smart_io_rq_meta *
smart_io_throttle_rq_meta(const struct request *rq)
{
	return READ_ONCE(((struct request *)rq)->elv.priv[1]);
}

static inline void smart_io_throttle_set_rq_meta(struct request *rq,
						 struct smart_io_rq_meta *meta)
{
	WRITE_ONCE(rq->elv.priv[1], meta);
}

static bool smart_io_throttle_is_normal_rw(const struct request *rq)
{
	enum req_op op = req_op((struct request *)rq);

	return !blk_rq_is_passthrough((struct request *)rq) &&
		(op == REQ_OP_READ || op == REQ_OP_WRITE) &&
		!(((struct request *)rq)->cmd_flags &
		  (REQ_PREFLUSH | REQ_FUA | REQ_META | REQ_ATOMIC));
}

static enum smart_io_rq_class
smart_io_throttle_classify_current(const struct request *rq)
{
	int fg_uid;
	u32 uid;

	if (!smart_io_throttle_is_normal_rw(rq))
		return SMART_IO_RQ_SPECIAL;

	if (req_op((struct request *)rq) == REQ_OP_READ &&
	    IOPRIO_PRIO_CLASS(req_get_ioprio((struct request *)rq)) ==
		    IOPRIO_CLASS_RT)
		return SMART_IO_RQ_FOREGROUND;

	fg_uid = smart_io_get_fg_uid();
	uid = from_kuid(&init_user_ns, current_uid());
	if (fg_uid >= 0 && uid == (u32)fg_uid)
		return SMART_IO_RQ_FOREGROUND;

	return SMART_IO_RQ_BACKGROUND;
}

enum smart_io_rq_class smart_io_throttle_rq_class(const struct request *rq)
{
	struct smart_io_rq_meta *meta;

	if (!rq)
		return SMART_IO_RQ_SPECIAL;
	meta = smart_io_throttle_rq_meta(rq);
	return meta ? meta->class : SMART_IO_RQ_SPECIAL;
}

static u32 *smart_io_throttle_pending_counter(struct smart_io_throttle_ctx *ctx,
						       enum smart_io_rq_class class)
{
	switch (class) {
	case SMART_IO_RQ_FOREGROUND:
		return &ctx->pending_fg;
	case SMART_IO_RQ_BACKGROUND:
		return &ctx->pending_bg;
	case SMART_IO_RQ_SPECIAL:
	default:
		return &ctx->pending_special;
	}
}

static void smart_io_throttle_pending_inc_locked(struct smart_io_throttle_ctx *ctx,
							  struct smart_io_rq_meta *meta)
{
	u32 *pending;

	if (meta->queued)
		return;
	pending = smart_io_throttle_pending_counter(ctx, meta->class);
	if (*pending == U32_MAX) {
		ctx->depth_anomalies++;
		return;
	}
	(*pending)++;
	meta->queued = true;
	if (meta->class == SMART_IO_RQ_BACKGROUND &&
	    smart_io_throttle_enabled())
		meta->queued_ts_ns = ktime_get_boottime_ns();
}

static void smart_io_throttle_pending_dec_locked(struct smart_io_throttle_ctx *ctx,
							  struct smart_io_rq_meta *meta)
{
	u32 *pending;

	if (!meta || !meta->queued)
		return;
	pending = smart_io_throttle_pending_counter(ctx, meta->class);
	if (!*pending)
		ctx->depth_anomalies++;
	else
		(*pending)--;
	meta->queued = false;
	meta->queued_ts_ns = 0;
}

static u64 smart_io_throttle_inflight_key(enum req_op op, sector_t end_sector)
{
	return ((u64)end_sector << 8) | (u8)op;
}

static void smart_io_throttle_inflight_remove_locked(struct smart_io_rq_meta *meta)
{
	if (!meta || !meta->inflight_indexed)
		return;
	hash_del(&meta->inflight_node);
	hash_del(&meta->inflight_start_node);
	meta->inflight_indexed = false;
}

static void smart_io_throttle_inflight_add_locked(
		struct smart_io_throttle_ctx *ctx, struct smart_io_rq_meta *meta,
		struct request *rq)
{
	if (!meta || meta->inflight_indexed ||
	    (meta->op != REQ_OP_READ && meta->op != REQ_OP_WRITE))
		return;

	meta->start_sector = blk_rq_pos(rq);
	meta->end_sector = meta->start_sector + blk_rq_sectors(rq);
	hash_add(ctx->inflight, &meta->inflight_node,
		 smart_io_throttle_inflight_key(meta->op, meta->end_sector));
	hash_add(ctx->inflight_start, &meta->inflight_start_node,
		 smart_io_throttle_inflight_key(meta->op, meta->start_sector));
	meta->inflight_indexed = true;
}

static void smart_io_throttle_clear_feedback_binding_locked(
		struct smart_io_throttle_ctx *ctx)
{
	if (ctx->feedback_meta)
		ctx->feedback_meta->feedback_bound = false;
	ctx->feedback_meta = NULL;
	ctx->feedback_dispatch_ts_ns = 0;
	ctx->feedback_issue_ts_ns = 0;
	ctx->feedback_state = SMART_IO_FEEDBACK_NONE;
}

static void smart_io_throttle_reset_feedback_locked(
		struct smart_io_throttle_ctx *ctx)
{
	smart_io_throttle_clear_feedback_binding_locked(ctx);
	ctx->feedback_vote_count = 0;
	ctx->feedback_fast_votes = 0;
	ctx->feedback_slow_votes = 0;
}

static void smart_io_throttle_reset_window_locked(struct smart_io_throttle_ctx *ctx)
{
	memset(ctx->dev_lat, 0, sizeof(ctx->dev_lat));
	ctx->dev_lat_head = 0;
	ctx->dev_lat_count = 0;
}

static void smart_io_throttle_set_baseline_action_locked(struct smart_io_throttle_ctx *ctx)
{
	ctx->action.level = SMART_IO_THROTTLE_NO;
	ctx->action.policy = SMART_IO_DISPATCH_BASELINE;
	ctx->action.ratio_pct = 0;
	ctx->action.queue_max = 0;
	ctx->action.target_depth = 0;
	ctx->action.decision_id = 0;
}

static bool smart_io_throttle_fallback_locked(struct smart_io_throttle_ctx *ctx)
{
	bool wake = ctx->gate_blocked;

	ctx->throttle_active = false;
	ctx->inference_pending = false;
	ctx->inference_timed_out = false;
	ctx->action_valid = false;
	ctx->pending_state_valid = false;
	ctx->gate_blocked = false;
	ctx->gate_reason = SMART_IO_GATE_NONE;
	smart_io_throttle_reset_feedback_locked(ctx);
	smart_io_throttle_reset_window_locked(ctx);
	smart_io_throttle_set_baseline_action_locked(ctx);
	return wake;
}

static bool smart_io_throttle_depth_can_run_locked(const struct smart_io_throttle_ctx *ctx)
{
	if (!ctx->throttle_active || !ctx->action_valid ||
	    ctx->action.level == SMART_IO_THROTTLE_NO)
		return true;

	return ctx->current_depth < ctx->action.target_depth;
}

static void smart_io_throttle_trace_decision(struct smart_io_throttle_ctx *ctx,
					      const char *trigger,
					      const char *result,
					      u64 session_id,
					      u64 control_id,
					      u64 decision_id,
					      const struct smart_io_action *action)
{
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("io_throttle_decision: ts_ns=%llu queue=%p session_id=%llu control_id=%llu decision_id=%llu trigger=%s result=%s source=%s level=%s policy=%s ratio_pct=%u queue_max=%u target_depth=%u current_depth=%u reserved_depth=%u issued_depth=%u\n",
			  ktime_get_boottime_ns(), ctx->queue, session_id,
			  control_id, decision_id, trigger, result,
			  smart_io_action_source_name(ctx->pending_source),
			  smart_io_throttle_level_name(action->level),
			  smart_io_dispatch_policy_name(action->policy),
			  action->ratio_pct, action->queue_max, action->target_depth,
			  READ_ONCE(ctx->current_depth), READ_ONCE(ctx->current_depth),
			  READ_ONCE(ctx->issued_depth));
}

static void smart_io_throttle_trace_state(
		struct smart_io_throttle_ctx *ctx,
		const struct smart_io_state_snapshot *state,
		u64 session_id, u64 control_id, u64 decision_id, bool valid)
{
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("io_rl_state: decision_ts_ns=%llu queue=%p session_id=%llu control_id=%llu decision_id=%llu valid=%u threshold_us=%u device_q_count=%u reserved_depth_count=%u device_q_fg_count=%u device_q_slow_count=%u queue_max=%u window_complete_count=%u window_mean_dev_us=%u window_span_us=%u window_slow_count=%u window_fg_complete_count=%u waiting_total_count=%u waiting_count=%u waiting_fg=%u waiting_high_ioprio_count=%u waiting_to_issued_contiguous_count=%u\n",
			  state->decision_ts_ns, ctx->queue, session_id,
			  control_id, decision_id, valid ? 1U : 0U,
			  smart_io_throttle_get_dev_lat_threshold(),
			  state->device_q_count, state->reserved_depth_count,
			  state->device_q_fg_count,
			  state->device_q_slow_count, state->queue_max,
			  state->window_complete_count,
			  state->window_mean_dev_us, state->window_span_us,
			  state->window_slow_count,
			  state->window_fg_complete_count,
			  state->waiting_total_count, state->waiting_count,
			  state->waiting_fg,
			  state->waiting_high_ioprio_count,
			  state->waiting_to_issued_contiguous_count);
}

static void smart_io_throttle_trace_dispatch(struct smart_io_throttle_ctx *ctx,
					      const struct smart_io_rq_meta *meta,
					      enum smart_io_dispatch_selection selection,
					      u32 depth_before, u32 depth_after,
					      u32 issued_depth_before,
					      u32 issued_depth_after,
					      bool feedback_bound,
					      u32 feedback_sample_no,
					      u64 queued_wait_us)
{
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("io_throttle_dispatch: ts_ns=%llu queue=%p session_id=%llu control_id=%llu decision_id=%llu rq_id=%llu rq_p=%p class=%u op=%u selection=%s level=%s policy=%s current_depth_before=%u current_depth_after=%u reserved_depth_before=%u reserved_depth_after=%u issued_depth_before=%u issued_depth_after=%u target_depth=%u feedback_bound=%u feedback_sample_no=%u queued_wait_us=%llu background_deadline_ms=%u\n",
			  ktime_get_boottime_ns(), ctx->queue, ctx->session_id,
			  ctx->control_id, meta ? meta->dispatch_decision_id : 0,
			  meta ? meta->rq_id : 0,
			  meta ? (void *)meta->rq : NULL,
			  meta ? meta->class : SMART_IO_RQ_SPECIAL,
			  meta ? meta->op : REQ_OP_FLUSH,
			  selection_names[selection],
			  smart_io_throttle_level_name(ctx->action.level),
			  smart_io_dispatch_policy_name(ctx->action.policy),
			  depth_before, depth_after, depth_before, depth_after,
			  issued_depth_before, issued_depth_after,
			  ctx->action.target_depth,
			  feedback_bound ? 1U : 0U, feedback_sample_no,
			  queued_wait_us,
			  smart_io_throttle_get_background_deadline_ms());
}

static void smart_io_throttle_trace_feedback(struct smart_io_throttle_ctx *ctx,
					      const struct smart_io_rq_meta *meta,
					      const struct smart_io_action *action,
					      const char *result, u64 issue_ns,
					      u64 complete_ns, u32 latency_us,
					      blk_status_t status, unsigned int nr_bytes,
					      u32 sample_no, u32 fast_votes,
					      u32 slow_votes)
{
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("io_throttle_feedback: queue=%p session_id=%llu control_id=%llu decision_id=%llu rq_id=%llu rq_p=%p issue_ts_ns=%llu completion_ts_ns=%llu dev_lat_us=%u threshold_us=%u status=%u nr_bytes=%u sample_no=%u fast_votes=%u slow_votes=%u votes_required=%u result=%s level=%s policy=%s target_depth=%u reserved_depth=%u issued_depth=%u\n",
			  ctx->queue, ctx->session_id, ctx->control_id,
			  meta ? meta->dispatch_decision_id : 0,
			  meta ? meta->rq_id : 0,
			  meta ? (void *)meta->rq : NULL,
			  issue_ns, complete_ns, latency_us,
			  smart_io_throttle_get_dev_lat_threshold(), (u32)status,
			  nr_bytes, sample_no, fast_votes, slow_votes,
			  SMART_IO_FEEDBACK_VOTES_REQUIRED, result,
			  smart_io_throttle_level_name(action ? action->level :
						 ctx->action.level),
			  smart_io_dispatch_policy_name(action ? action->policy :
						 ctx->action.policy),
			  action ? action->target_depth : ctx->action.target_depth,
			  READ_ONCE(ctx->current_depth), READ_ONCE(ctx->issued_depth));
}

static void smart_io_throttle_trace_gate(struct smart_io_throttle_ctx *ctx,
					  const char *event,
					  enum smart_io_gate_reason reason,
					  u32 current_depth, u32 target_depth)
{
	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0))
		return;

	smart_io_raw_emit("io_throttle_gate: ts_ns=%llu queue=%p session_id=%llu control_id=%llu decision_id=%llu event=%s reason=%s current_depth=%u reserved_depth=%u issued_depth=%u target_depth=%u bg_pending=%u\n",
			  ktime_get_boottime_ns(), ctx->queue, ctx->session_id,
			  ctx->control_id, ctx->decision_id, event,
			  smart_io_gate_reason_name(reason), current_depth, current_depth,
			  READ_ONCE(ctx->issued_depth), target_depth,
			  READ_ONCE(ctx->pending_bg));
}

static struct smart_io_rq_meta *
smart_io_throttle_find_meta_locked(struct smart_io_throttle_ctx *ctx,
				   struct request *rq)
{
	struct smart_io_rq_meta *meta;
	u32 budget = ctx->meta_capacity;

	hash_for_each_possible(ctx->active, meta, active_node,
			       (unsigned long)rq) {
		if (!budget--)
			break;
		if (meta->rq == rq)
			return meta;
	}
	return NULL;
}

static struct smart_io_rq_meta *
smart_io_throttle_alloc_meta_locked(struct smart_io_throttle_ctx *ctx,
					    struct request *rq,
					    enum smart_io_rq_class class)
{
	struct smart_io_rq_meta *meta;

	if (list_empty(&ctx->free_meta)) {
		ctx->allocation_failures++;
		ctx->depth_anomalies++;
		smart_io_throttle_fallback_locked(ctx);
		return NULL;
	}

	meta = list_first_entry(&ctx->free_meta, struct smart_io_rq_meta,
				free_node);
	list_del_init(&meta->free_node);
	memset(meta, 0, sizeof(*meta));
	INIT_LIST_HEAD(&meta->free_node);
	INIT_HLIST_NODE(&meta->active_node);
	INIT_HLIST_NODE(&meta->inflight_node);
	INIT_HLIST_NODE(&meta->inflight_start_node);
	meta->rq = rq;
	meta->class = class;
	meta->op = req_op(rq);
	meta->start_sector = blk_rq_pos(rq);
	meta->end_sector = meta->start_sector + blk_rq_sectors(rq);
	meta->high_ioprio =
		IOPRIO_PRIO_CLASS(req_get_ioprio(rq)) == IOPRIO_CLASS_RT;
	meta->rq_id = ++ctx->next_rq_id;
	hash_add(ctx->active, &meta->active_node, (unsigned long)rq);
	ctx->meta_inuse++;
	if (rq->rq_flags & RQF_USE_SCHED)
		smart_io_throttle_set_rq_meta(rq, meta);
	return meta;
}

static void smart_io_throttle_free_meta_locked(struct smart_io_throttle_ctx *ctx,
						   struct request *rq,
						   struct smart_io_rq_meta *meta)
{
	if (!meta)
		return;
	if (!ctx->meta_inuse) {
		ctx->depth_anomalies++;
		smart_io_throttle_fallback_locked(ctx);
		return;
	}

	smart_io_throttle_pending_dec_locked(ctx, meta);
	smart_io_throttle_inflight_remove_locked(meta);
	if (ctx->feedback_meta == meta)
		smart_io_throttle_reset_feedback_locked(ctx);
	if (rq && (rq->rq_flags & RQF_USE_SCHED) &&
	    smart_io_throttle_rq_meta(rq) == meta)
		smart_io_throttle_set_rq_meta(rq, NULL);
	if (!hlist_unhashed(&meta->active_node))
		hash_del(&meta->active_node);
	ctx->meta_inuse--;
	memset(meta, 0, sizeof(*meta));
	INIT_LIST_HEAD(&meta->free_node);
	INIT_HLIST_NODE(&meta->active_node);
	INIT_HLIST_NODE(&meta->inflight_node);
	INIT_HLIST_NODE(&meta->inflight_start_node);
	list_add_tail(&meta->free_node, &ctx->free_meta);
}

static bool smart_io_throttle_release_depth_locked(struct smart_io_throttle_ctx *ctx,
						     bool *depth_accounted,
						     enum smart_io_gate_reason *released_reason)
{
	if (!depth_accounted || !*depth_accounted)
		return false;

	*depth_accounted = false;
	if (!ctx->current_depth) {
		ctx->depth_anomalies++;
		if (ctx->throttle_active)
			smart_io_throttle_fallback_locked(ctx);
		return true;
	}

	ctx->current_depth--;
	if (ctx->gate_blocked && smart_io_throttle_depth_can_run_locked(ctx)) {
		*released_reason = ctx->gate_reason;
		ctx->gate_blocked = false;
		ctx->gate_reason = SMART_IO_GATE_NONE;
		return true;
	}

	return false;
}

static bool
smart_io_throttle_account_depth_locked(struct smart_io_throttle_ctx *ctx,
				       bool *depth_accounted)
{
	if (!depth_accounted || *depth_accounted) {
		ctx->depth_anomalies++;
		smart_io_throttle_fallback_locked(ctx);
		return true;
	}
	if (ctx->current_depth == U32_MAX) {
		ctx->depth_anomalies++;
		smart_io_throttle_fallback_locked(ctx);
		return true;
	}

	*depth_accounted = true;
	ctx->current_depth++;
	return false;
}

static bool
smart_io_throttle_account_issued_depth_locked(
		struct smart_io_throttle_ctx *ctx, bool *issued_accounted)
{
	if (!issued_accounted || *issued_accounted) {
		ctx->depth_anomalies++;
		smart_io_throttle_fallback_locked(ctx);
		return true;
	}
	if (ctx->issued_depth == U32_MAX) {
		ctx->depth_anomalies++;
		smart_io_throttle_fallback_locked(ctx);
		return true;
	}

	*issued_accounted = true;
	ctx->issued_depth++;
	return false;
}

static bool
smart_io_throttle_release_issued_depth_locked(
		struct smart_io_throttle_ctx *ctx, bool *issued_accounted)
{
	if (!issued_accounted || !*issued_accounted)
		return false;

	*issued_accounted = false;
	if (!ctx->issued_depth) {
		ctx->depth_anomalies++;
		if (ctx->throttle_active)
			return smart_io_throttle_fallback_locked(ctx);
		return false;
	}
	ctx->issued_depth--;
	return false;
}

static bool smart_io_throttle_release_deadline_locked(
		struct smart_io_throttle_ctx *ctx, struct smart_io_rq_meta *meta)
{
	if (!meta || !meta->deadline_forced)
		return false;

	meta->deadline_forced = false;
	if (!ctx->deadline_forced_active) {
		ctx->depth_anomalies++;
		return false;
	}
	ctx->deadline_forced_active = false;
	return true;
}

static void smart_io_throttle_snapshot_config_locked(struct smart_io_throttle_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&throttle_config_lock, flags);
	ctx->pending_source = throttle_config.source;
	ctx->pending_fixed_action = throttle_config.fixed_action;
	ctx->pending_light_pct = throttle_config.light_pct;
	ctx->pending_medium_pct = throttle_config.medium_pct;
	ctx->pending_heavy_pct = throttle_config.heavy_pct;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
}

static enum smart_io_throttle_level
smart_io_throttle_next_monotonic_level(struct smart_io_throttle_ctx *ctx,
				       bool new_control)
{
	if (new_control)
		return SMART_IO_THROTTLE_LIGHT;

	switch (ctx->action.level) {
	case SMART_IO_THROTTLE_LIGHT:
		return SMART_IO_THROTTLE_MEDIUM;
	case SMART_IO_THROTTLE_MEDIUM:
	case SMART_IO_THROTTLE_HEAVY:
		return SMART_IO_THROTTLE_HEAVY;
	case SMART_IO_THROTTLE_NO:
	default:
		return SMART_IO_THROTTLE_LIGHT;
	}
}

static bool smart_io_throttle_start_inference_locked(struct smart_io_throttle_ctx *ctx,
						     bool new_control)
{
	struct smart_io_state_snapshot state;
	u32 queue_max;
	u32 threshold_us;
	u64 now_ns;

	if (!smart_io_throttle_enabled() || ctx->dying ||
	    ctx->inference_pending)
		return false;
	queue_max = blk_queue_depth(ctx->queue);
	threshold_us = smart_io_throttle_get_dev_lat_threshold();
	now_ns = ktime_get_boottime_ns();
	if (!smart_io_throttle_snapshot_state_locked(ctx, &state, now_ns,
						     queue_max, threshold_us))
		return false;

	if (new_control)
		ctx->control_id++;
	ctx->throttle_active = true;
	ctx->inference_pending = true;
	ctx->inference_timed_out = false;
	ctx->action_valid = false;
	ctx->pending_state_valid = false;
	smart_io_throttle_reset_feedback_locked(ctx);
	ctx->decision_id++;
	ctx->pending_session_id = ctx->session_id;
	ctx->pending_control_id = ctx->control_id;
	ctx->pending_decision_id = ctx->decision_id;
	ctx->pending_state = state;
	ctx->pending_state_valid = true;
	smart_io_throttle_snapshot_config_locked(ctx);
	if (ctx->pending_source == SMART_IO_ACTION_SOURCE_MONOTONIC) {
		ctx->pending_fixed_action.level =
			smart_io_throttle_next_monotonic_level(ctx, new_control);
		ctx->pending_fixed_action.policy = SMART_IO_DISPATCH_BASELINE;
	}
	return true;
}

static void smart_io_throttle_schedule_inference(struct smart_io_throttle_ctx *ctx)
{
	unsigned long flags;
	bool wake = false;

	hrtimer_start(&ctx->inference_timer,
		      ms_to_ktime(SMART_IO_INFERENCE_TIMEOUT_MS),
		      HRTIMER_MODE_REL);
	if (queue_work(ctx->inference_wq, &ctx->inference_work))
		return;

	hrtimer_cancel(&ctx->inference_timer);
	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->inference_pending)
		wake = smart_io_throttle_fallback_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
}

static bool smart_io_throttle_action_valid(const struct smart_io_action *action)
{
	return action->level < SMART_IO_THROTTLE_LEVEL_COUNT &&
		action->policy < SMART_IO_DISPATCH_POLICY_COUNT;
}

static u32 smart_io_throttle_action_ratio(const struct smart_io_throttle_ctx *ctx,
						   enum smart_io_throttle_level level)
{
	switch (level) {
	case SMART_IO_THROTTLE_LIGHT:
		return ctx->pending_light_pct;
	case SMART_IO_THROTTLE_MEDIUM:
		return ctx->pending_medium_pct;
	case SMART_IO_THROTTLE_HEAVY:
		return ctx->pending_heavy_pct;
	case SMART_IO_THROTTLE_NO:
	default:
		return 0;
	}
}

static void smart_io_throttle_inference_work(struct work_struct *work)
{
	struct smart_io_throttle_ctx *ctx =
		container_of(work, struct smart_io_throttle_ctx, inference_work);
	struct smart_io_action action;
	struct smart_io_state_snapshot state;
	unsigned long flags;
	u64 session_id;
	u64 control_id;
	u64 decision_id;
	enum smart_io_action_source source;
	u32 queue_max;
	u32 ratio_pct;
	bool wake = false;
	int ret = 0;

	spin_lock_irqsave(&ctx->lock, flags);
	if (!ctx->inference_pending || ctx->inference_timed_out || ctx->dying) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return;
	}
	session_id = ctx->pending_session_id;
	control_id = ctx->pending_control_id;
	decision_id = ctx->pending_decision_id;
	source = ctx->pending_source;
	action = ctx->pending_fixed_action;
	state = ctx->pending_state;
	if (!ctx->pending_state_valid) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);
	smart_io_throttle_trace_state(ctx, &state, session_id, control_id,
				      decision_id, true);

	if (source == SMART_IO_ACTION_SOURCE_MODEL)
		ret = -EOPNOTSUPP;
	else if (!smart_io_throttle_action_valid(&action))
		ret = -EINVAL;

	if (ret)
		goto fallback;

	queue_max = blk_queue_depth(ctx->queue);
	if (!queue_max) {
		ret = -ERANGE;
		goto fallback;
	}

	spin_lock_irqsave(&ctx->lock, flags);
	if (!ctx->inference_pending || ctx->inference_timed_out || ctx->dying ||
	    session_id != ctx->pending_session_id ||
	    control_id != ctx->pending_control_id ||
	    decision_id != ctx->pending_decision_id) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return;
	}

	ratio_pct = smart_io_throttle_action_ratio(ctx, action.level);
	if (action.level != SMART_IO_THROTTLE_NO &&
	    (!ratio_pct || ratio_pct > 100)) {
		ctx->invalid_result_count++;
		wake = smart_io_throttle_fallback_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);
		smart_io_throttle_trace_decision(ctx, "inference", "invalid",
					  session_id, control_id, decision_id,
					  &action);
		if (wake)
			blk_mq_run_hw_queues(ctx->queue, true);
		return;
	}

	action.ratio_pct = ratio_pct;
	action.queue_max = queue_max;
	action.target_depth = action.level == SMART_IO_THROTTLE_NO ? 0 :
		min_t(u64, queue_max,
		      DIV_ROUND_UP((u64)queue_max * ratio_pct, 100));
	if (action.level != SMART_IO_THROTTLE_NO && !action.target_depth) {
		ctx->invalid_result_count++;
		wake = smart_io_throttle_fallback_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);
		smart_io_throttle_trace_decision(ctx, "inference", "invalid",
					  session_id, control_id, decision_id,
					  &action);
		if (wake)
			blk_mq_run_hw_queues(ctx->queue, true);
		return;
	}

	action.decision_id = decision_id;
	ctx->action = action;
	ctx->action_valid = true;
	ctx->inference_pending = false;
	ctx->inference_timed_out = false;
	ctx->feedback_state = SMART_IO_FEEDBACK_WAIT_DISPATCH;
	ctx->inference_count++;
	ctx->gate_blocked = false;
	ctx->gate_reason = SMART_IO_GATE_NONE;
	spin_unlock_irqrestore(&ctx->lock, flags);

	hrtimer_cancel(&ctx->inference_timer);
	smart_io_throttle_trace_decision(ctx, "inference", "applied",
					  session_id, control_id, decision_id, &action);
	blk_mq_run_hw_queues(ctx->queue, true);
	return;

fallback:
	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->inference_pending && !ctx->inference_timed_out &&
	    session_id == ctx->pending_session_id &&
	    control_id == ctx->pending_control_id &&
	    decision_id == ctx->pending_decision_id) {
		ctx->invalid_result_count++;
		wake = smart_io_throttle_fallback_locked(ctx);
	}
	spin_unlock_irqrestore(&ctx->lock, flags);
	smart_io_throttle_trace_decision(ctx, "inference", "provider_error",
					  session_id, control_id, decision_id, &action);
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
}

static void smart_io_throttle_timeout_work(struct work_struct *work)
{
	struct smart_io_throttle_ctx *ctx =
		container_of(work, struct smart_io_throttle_ctx, timeout_work);
	struct smart_io_action action;
	unsigned long flags;
	u64 session_id = 0;
	u64 control_id = 0;
	u64 decision_id = 0;
	bool wake = false;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->inference_pending && ctx->inference_timed_out) {
		session_id = ctx->pending_session_id;
		control_id = ctx->pending_control_id;
		decision_id = ctx->pending_decision_id;
		action = ctx->pending_fixed_action;
		ctx->timeout_count++;
		wake = smart_io_throttle_fallback_locked(ctx);
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (!session_id)
		return;
	smart_io_throttle_trace_decision(ctx, "inference", "timeout",
					  session_id, control_id, decision_id, &action);
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
}

static enum hrtimer_restart
smart_io_throttle_inference_timer(struct hrtimer *timer)
{
	struct smart_io_throttle_ctx *ctx =
		container_of(timer, struct smart_io_throttle_ctx, inference_timer);
	unsigned long flags;
	bool queue_timeout = false;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->inference_pending && !ctx->dying) {
		ctx->inference_timed_out = true;
		queue_timeout = true;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (queue_timeout)
		queue_work(ctx->timeout_wq, &ctx->timeout_work);

	return HRTIMER_NORESTART;
}

static int smart_io_throttle_meta_capacity(struct request_queue *q,
					   u32 *capacity)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;
	u32 tag_slots = 0;
	u32 flush_slots = q->nr_hw_queues;
	u32 total;

	if (q->sched_shared_tags) {
		tag_slots = q->sched_shared_tags->nr_tags;
	} else {
		queue_for_each_hw_ctx(q, hctx, i) {
			if (!hctx->sched_tags ||
			    check_add_overflow(tag_slots,
					       hctx->sched_tags->nr_tags,
					       &tag_slots))
				return -EOVERFLOW;
		}
	}

	if (!tag_slots || !flush_slots ||
	    check_add_overflow(tag_slots, flush_slots, &total))
		return -EOVERFLOW;
	*capacity = total;
	return 0;
}

int smart_io_throttle_queue_init(struct smart_io_throttle_ctx *ctx,
				 struct request_queue *q)
{
	size_t pool_bytes;
	u32 i;
	u32 capacity;
	int ret;

	memset(ctx, 0, sizeof(*ctx));
	ctx->queue = q;
	ret = smart_io_throttle_meta_capacity(q, &capacity);
	if (ret)
		return ret;
	if (check_mul_overflow((size_t)capacity, sizeof(*ctx->meta_pool),
			       &pool_bytes))
		return -EOVERFLOW;
	ctx->meta_pool = kzalloc(pool_bytes, GFP_KERNEL);
	if (!ctx->meta_pool)
		return -ENOMEM;

	ctx->meta_capacity = capacity;
	INIT_LIST_HEAD(&ctx->active_node);
	INIT_LIST_HEAD(&ctx->free_meta);
	spin_lock_init(&ctx->lock);
	hash_init(ctx->active);
	hash_init(ctx->inflight);
	hash_init(ctx->inflight_start);
	for (i = 0; i < capacity; i++) {
		INIT_LIST_HEAD(&ctx->meta_pool[i].free_node);
		INIT_HLIST_NODE(&ctx->meta_pool[i].active_node);
		INIT_HLIST_NODE(&ctx->meta_pool[i].inflight_node);
		INIT_HLIST_NODE(&ctx->meta_pool[i].inflight_start_node);
		list_add_tail(&ctx->meta_pool[i].free_node, &ctx->free_meta);
	}
	smart_io_throttle_set_baseline_action_locked(ctx);
	INIT_WORK(&ctx->inference_work, smart_io_throttle_inference_work);
	INIT_WORK(&ctx->timeout_work, smart_io_throttle_timeout_work);
	hrtimer_init(&ctx->inference_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ctx->inference_timer.function = smart_io_throttle_inference_timer;
	ctx->inference_wq = alloc_workqueue("smart_io_infer",
					  WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!ctx->inference_wq)
		goto free_meta;
	ctx->timeout_wq = alloc_workqueue("smart_io_timeout",
					WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!ctx->timeout_wq)
		goto destroy_inference_wq;

	mutex_lock(&active_throttle_queues_lock);
	list_add_tail_rcu(&ctx->active_node, &active_throttle_queues);
	mutex_unlock(&active_throttle_queues_lock);
	return 0;

destroy_inference_wq:
	destroy_workqueue(ctx->inference_wq);
	ctx->inference_wq = NULL;
free_meta:
	kfree(ctx->meta_pool);
	ctx->meta_pool = NULL;
	return -ENOMEM;
}

void smart_io_throttle_queue_exit(struct smart_io_throttle_ctx *ctx)
{
	unsigned long flags;
	bool meta_busy;
	bool active_busy;

	mutex_lock(&active_throttle_queues_lock);
	ctx->dying = true;
	list_del_rcu(&ctx->active_node);
	mutex_unlock(&active_throttle_queues_lock);
	synchronize_rcu();

	hrtimer_cancel(&ctx->inference_timer);
	cancel_work_sync(&ctx->inference_work);
	cancel_work_sync(&ctx->timeout_work);
	if (ctx->inference_wq)
		destroy_workqueue(ctx->inference_wq);
	if (ctx->timeout_wq)
		destroy_workqueue(ctx->timeout_wq);

	spin_lock_irqsave(&ctx->lock, flags);
	WARN_ON_ONCE(ctx->current_depth != 0);
	WARN_ON_ONCE(ctx->issued_depth != 0);
	WARN_ON_ONCE(ctx->pending_special || ctx->pending_fg || ctx->pending_bg);
	WARN_ON_ONCE(ctx->deadline_forced_active);
	active_busy = WARN_ON_ONCE(!hash_empty(ctx->active));
	meta_busy = WARN_ON_ONCE(ctx->meta_inuse != 0);
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (meta_busy || active_busy)
		return;
	kfree(ctx->meta_pool);
	ctx->meta_pool = NULL;
}

void smart_io_throttle_prepare_request(struct request *rq)
{
	if (rq)
		smart_io_throttle_set_rq_meta(rq, NULL);
}

void smart_io_throttle_classify_request(struct smart_io_throttle_ctx *ctx,
					struct request *rq)
{
	struct smart_io_rq_meta *meta;
	unsigned long flags;

	if (!ctx || !rq)
		return;

	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_rq_meta(rq);
	if (!meta)
		meta = smart_io_throttle_alloc_meta_locked(ctx, rq,
						   smart_io_throttle_classify_current(rq));
	if (meta)
		smart_io_throttle_pending_inc_locked(ctx, meta);
	spin_unlock_irqrestore(&ctx->lock, flags);
}

void smart_io_throttle_update_request(struct smart_io_throttle_ctx *ctx,
				      struct request *rq)
{
	struct smart_io_rq_meta *meta;
	unsigned long flags;

	if (!ctx || !rq)
		return;

	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_rq_meta(rq);
	if (meta && !meta->issued_accounted) {
		meta->op = req_op(rq);
		meta->start_sector = blk_rq_pos(rq);
		meta->end_sector = meta->start_sector + blk_rq_sectors(rq);
		meta->high_ioprio =
			IOPRIO_PRIO_CLASS(req_get_ioprio(rq)) == IOPRIO_CLASS_RT;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);
}

void smart_io_throttle_unqueue_request(struct smart_io_throttle_ctx *ctx,
					struct request *rq)
{
	struct smart_io_rq_meta *meta;
	unsigned long flags;

	if (!ctx || !rq)
		return;

	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_rq_meta(rq);
	smart_io_throttle_pending_dec_locked(ctx, meta);
	spin_unlock_irqrestore(&ctx->lock, flags);
}

void smart_io_throttle_account_dispatch(struct smart_io_throttle_ctx *ctx,
					struct request *rq,
					enum smart_io_dispatch_selection selection)
{
	struct smart_io_rq_meta *meta;
	enum smart_io_rq_class class = SMART_IO_RQ_SPECIAL;
	unsigned long flags;
	u32 depth_before = 0;
	u32 depth_after = 0;
	u32 issued_depth_before = 0;
	u32 issued_depth_after = 0;
	u32 feedback_sample_no = 0;
	u64 now_ns;
	u64 queued_wait_us = 0;
	bool feedback_bound = false;
	bool wake = false;

	if (!ctx || !rq)
		return;

	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_rq_meta(rq);
	if (!meta) {
		ctx->depth_anomalies++;
		wake = smart_io_throttle_fallback_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);
		if (wake)
			blk_mq_run_hw_queues(ctx->queue, true);
		return;
	}

	class = meta->class;
	now_ns = ktime_get_boottime_ns();
	if (meta->queued_ts_ns && now_ns > meta->queued_ts_ns)
		queued_wait_us = div_u64(now_ns - meta->queued_ts_ns,
					 NSEC_PER_USEC);
	smart_io_throttle_pending_dec_locked(ctx, meta);
	meta->dispatch_decision_id = 0;
	if (smart_io_throttle_enabled() && ctx->throttle_active &&
	    ctx->action_valid && !ctx->inference_pending)
		meta->dispatch_decision_id = ctx->action.decision_id;
	depth_before = ctx->current_depth;
	issued_depth_before = ctx->issued_depth;
	wake = smart_io_throttle_account_depth_locked(ctx,
						      &meta->depth_accounted);

	switch (class) {
	case SMART_IO_RQ_SPECIAL:
		ctx->special_dispatched++;
		break;
	case SMART_IO_RQ_FOREGROUND:
		ctx->fg_dispatched++;
		break;
	case SMART_IO_RQ_BACKGROUND:
		ctx->bg_dispatched++;
		break;
	}

	switch (selection) {
	case SMART_IO_SELECTION_BACKGROUND_BASELINE:
		ctx->baseline_hits++;
		break;
	case SMART_IO_SELECTION_BACKGROUND_DEADLINE:
		if (ctx->deadline_forced_active) {
			ctx->depth_anomalies++;
		} else {
			meta->deadline_forced = true;
			ctx->deadline_forced_active = true;
			ctx->bg_deadline_forced++;
		}
		break;
	case SMART_IO_SELECTION_SEQ_HIT:
		ctx->seq_hits++;
		break;
	case SMART_IO_SELECTION_SEQ_FALLBACK:
		ctx->seq_fallbacks++;
		break;
	case SMART_IO_SELECTION_SMALL:
		ctx->small_hits++;
		break;
	default:
		break;
	}

	if (smart_io_throttle_enabled() && ctx->throttle_active &&
	    ctx->action_valid && !ctx->inference_pending &&
	    ctx->feedback_state == SMART_IO_FEEDBACK_WAIT_DISPATCH &&
	    ctx->feedback_vote_count < SMART_IO_FEEDBACK_SAMPLE_MAX &&
	    class != SMART_IO_RQ_SPECIAL) {
		ctx->feedback_meta = meta;
		ctx->feedback_dispatch_ts_ns = ktime_get_boottime_ns();
		ctx->feedback_issue_ts_ns = 0;
		ctx->feedback_state = SMART_IO_FEEDBACK_WAIT_ISSUE;
		meta->feedback_bound = true;
		meta->dispatch_decision_id = ctx->action.decision_id;
		feedback_sample_no = ctx->feedback_vote_count + 1;
		feedback_bound = true;
	}

	depth_after = ctx->current_depth;
	issued_depth_after = ctx->issued_depth;
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (smart_io_throttle_enabled())
		smart_io_throttle_trace_dispatch(ctx, meta, selection, depth_before,
						 depth_after, issued_depth_before,
						 issued_depth_after, feedback_bound,
						 feedback_sample_no,
						 queued_wait_us);
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
}

static void smart_io_throttle_invalidate_feedback_locked(
		struct smart_io_throttle_ctx *ctx, struct smart_io_rq_meta *meta,
		const char **result)
{
	if (ctx->feedback_meta != meta)
		return;

	*result = ctx->feedback_state == SMART_IO_FEEDBACK_WAIT_ISSUE ?
		"invalid_preissue" : "invalid_requeue";
	smart_io_throttle_clear_feedback_binding_locked(ctx);
	if (ctx->throttle_active && ctx->action_valid &&
	    !ctx->inference_pending)
		ctx->feedback_state = SMART_IO_FEEDBACK_WAIT_DISPATCH;
	ctx->feedback_rebind_count++;
}

void smart_io_throttle_finish_request(struct smart_io_throttle_ctx *ctx,
					struct request *rq)
{
	struct smart_io_rq_meta *meta;
	struct smart_io_rq_meta feedback_meta;
	enum smart_io_gate_reason released_reason = SMART_IO_GATE_NONE;
	unsigned long flags;
	const char *feedback_result = NULL;
	u32 feedback_sample_no = 0;
	u32 feedback_fast_votes = 0;
	u32 feedback_slow_votes = 0;
	bool trace_feedback = false;
	bool wake = false;

	if (!ctx || !rq)
		return;

	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_rq_meta(rq);
	if (!meta) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return;
	}
	if (ctx->feedback_meta == meta) {
		feedback_sample_no = ctx->feedback_vote_count + 1;
		feedback_fast_votes = ctx->feedback_fast_votes;
		feedback_slow_votes = ctx->feedback_slow_votes;
	}
	smart_io_throttle_invalidate_feedback_locked(ctx, meta, &feedback_result);
	if (feedback_result) {
		feedback_meta = *meta;
		trace_feedback = true;
	}
	smart_io_throttle_inflight_remove_locked(meta);
	wake |= smart_io_throttle_release_deadline_locked(ctx, meta);
	wake |= smart_io_throttle_release_issued_depth_locked(ctx,
								    &meta->issued_accounted);
	wake |= smart_io_throttle_release_depth_locked(ctx,
					       &meta->depth_accounted,
					       &released_reason);
	smart_io_throttle_free_meta_locked(ctx, rq, meta);
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (trace_feedback)
		smart_io_throttle_trace_feedback(ctx, &feedback_meta, &ctx->action,
						 feedback_result,
						 0, 0, 0,
						 BLK_STS_OK, 0,
						 feedback_sample_no,
						 feedback_fast_votes,
						 feedback_slow_votes);
	if (wake) {
		smart_io_throttle_trace_gate(ctx, "release", released_reason,
					      READ_ONCE(ctx->current_depth),
					      READ_ONCE(ctx->action.target_depth));
		blk_mq_run_hw_queues(ctx->queue, true);
	}
}

bool smart_io_throttle_enabled(void)
{
	return atomic_read(&throttle_enabled) != 0;
}

bool smart_io_throttle_has_pending(struct smart_io_throttle_ctx *ctx,
				   enum smart_io_rq_class class)
{
	unsigned long flags;
	u32 pending;

	if (!ctx)
		return false;
	spin_lock_irqsave(&ctx->lock, flags);
	pending = *smart_io_throttle_pending_counter(ctx, class);
	spin_unlock_irqrestore(&ctx->lock, flags);
	return pending != 0;
}

bool smart_io_throttle_background_allowed(struct smart_io_throttle_ctx *ctx)
{
	enum smart_io_gate_reason reason = SMART_IO_GATE_NONE;
	unsigned long flags;
	u32 current_depth;
	u32 target_depth;
	bool allowed = true;
	bool trace_enter = false;

	if (!ctx || !smart_io_throttle_enabled())
		return true;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->inference_pending) {
		allowed = false;
		reason = SMART_IO_GATE_INFERENCE;
	} else if (!smart_io_throttle_depth_can_run_locked(ctx)) {
		allowed = false;
		reason = SMART_IO_GATE_DEPTH;
	}
	current_depth = ctx->current_depth;
	target_depth = ctx->action.target_depth;
	if (!allowed && (!ctx->gate_blocked || ctx->gate_reason != reason)) {
		ctx->gate_blocked = true;
		ctx->gate_reason = reason;
		ctx->bg_blocked++;
		trace_enter = true;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (trace_enter)
		smart_io_throttle_trace_gate(ctx, "enter", reason, current_depth,
					      target_depth);
	return allowed;
}

bool smart_io_throttle_deadline_escape_available(
		struct smart_io_throttle_ctx *ctx)
{
	unsigned long flags;
	bool available;

	if (!ctx || !smart_io_throttle_enabled())
		return false;
	spin_lock_irqsave(&ctx->lock, flags);
	available = !ctx->deadline_forced_active;
	spin_unlock_irqrestore(&ctx->lock, flags);
	return available;
}

bool smart_io_throttle_background_deadline_expired(
		struct smart_io_throttle_ctx *ctx, const struct request *rq,
		u64 now_ns, u64 *queued_ts_ns)
{
	struct smart_io_rq_meta *meta;
	u64 deadline_ns;
	u64 queued_ns;

	if (!ctx || !rq || !smart_io_throttle_enabled())
		return false;
	meta = smart_io_throttle_rq_meta(rq);
	if (!meta || meta->class != SMART_IO_RQ_BACKGROUND || !meta->queued)
		return false;

	queued_ns = READ_ONCE(meta->queued_ts_ns);
	if (!queued_ns || now_ns <= queued_ns)
		return false;
	deadline_ns = (u64)smart_io_throttle_get_background_deadline_ms() *
		NSEC_PER_MSEC;
	if (now_ns - queued_ns < deadline_ns)
		return false;
	if (queued_ts_ns)
		*queued_ts_ns = queued_ns;
	return true;
}

enum smart_io_dispatch_policy
smart_io_throttle_effective_policy(struct smart_io_throttle_ctx *ctx)
{
	enum smart_io_dispatch_policy policy = SMART_IO_DISPATCH_BASELINE;
	unsigned long flags;

	if (!ctx || !smart_io_throttle_enabled())
		return policy;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->throttle_active && ctx->action_valid &&
	    !ctx->inference_pending)
		policy = ctx->action.policy;
	spin_unlock_irqrestore(&ctx->lock, flags);
	return policy;
}

bool smart_io_throttle_seq_match(struct smart_io_throttle_ctx *ctx,
				  const struct request *rq)
{
	struct smart_io_rq_meta *meta;
	enum req_op op;
	sector_t start_sector;
	u64 key;
	unsigned long flags;
	u32 budget;

	if (!ctx || !rq || smart_io_throttle_rq_class(rq) !=
		    SMART_IO_RQ_BACKGROUND)
		return false;

	op = req_op((struct request *)rq);
	if (op != REQ_OP_READ && op != REQ_OP_WRITE)
		return false;
	start_sector = blk_rq_pos((struct request *)rq);
	key = smart_io_throttle_inflight_key(op, start_sector);
	budget = smart_io_throttle_scan_budget(ctx);

	spin_lock_irqsave(&ctx->lock, flags);
	hash_for_each_possible(ctx->inflight, meta, inflight_node, key) {
		if (!budget--)
			break;
		if (meta->op == op && meta->end_sector == start_sector) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			return true;
		}
	}
	spin_unlock_irqrestore(&ctx->lock, flags);
	return false;
}

u32 smart_io_throttle_scan_budget(const struct smart_io_throttle_ctx *ctx)
{
	return ctx ? max_t(u32, 1, READ_ONCE(ctx->meta_capacity)) : 1;
}

static bool smart_io_throttle_contiguous_locked(
		struct smart_io_throttle_ctx *ctx,
		const struct smart_io_rq_meta *waiting)
{
	struct smart_io_rq_meta *issued;
	u64 key;
	u32 budget = ctx->meta_capacity;

	if (!waiting ||
	    (waiting->op != REQ_OP_READ && waiting->op != REQ_OP_WRITE))
		return false;

	key = smart_io_throttle_inflight_key(waiting->op,
					     waiting->start_sector);
	hash_for_each_possible(ctx->inflight, issued, inflight_node, key) {
		if (!budget--)
			break;
		if (issued->issued_accounted && issued->op == waiting->op &&
		    issued->end_sector == waiting->start_sector)
			return true;
	}

	budget = ctx->meta_capacity;
	key = smart_io_throttle_inflight_key(waiting->op,
					     waiting->end_sector);
	hash_for_each_possible(ctx->inflight_start, issued, inflight_start_node,
			       key) {
		if (!budget--)
			break;
		if (issued->issued_accounted && issued->op == waiting->op &&
		    issued->start_sector == waiting->end_sector)
			return true;
	}

	return false;
}

static bool smart_io_throttle_window_snapshot_locked(
		struct smart_io_throttle_ctx *ctx,
		struct smart_io_state_snapshot *state, u32 threshold_us)
{
	const struct smart_io_completion_sample *oldest;
	const struct smart_io_completion_sample *sample;
	u64 latency_sum = 0;
	u64 span_ns;
	u8 i;

	if (ctx->dev_lat_count != SMART_IO_DEV_LAT_WINDOW)
		return false;

	oldest = &ctx->dev_lat[ctx->dev_lat_head];
	sample = &ctx->dev_lat[(ctx->dev_lat_head +
				 SMART_IO_DEV_LAT_WINDOW - 1) %
				SMART_IO_DEV_LAT_WINDOW];
	if (!oldest->complete_ts_ns ||
	    sample->complete_ts_ns < oldest->complete_ts_ns)
		return false;
	span_ns = sample->complete_ts_ns - oldest->complete_ts_ns;
	if (span_ns > (u64)SMART_IO_DEV_LAT_WINDOW_MAX_US * NSEC_PER_USEC)
		return false;

	state->window_complete_count = SMART_IO_DEV_LAT_WINDOW;
	state->window_span_us = div_u64(span_ns, NSEC_PER_USEC);
	for (i = 0; i < SMART_IO_DEV_LAT_WINDOW; i++) {
		sample = &ctx->dev_lat[i];
		latency_sum += sample->dev_lat_us;
		if (sample->dev_lat_us >= threshold_us)
			state->window_slow_count++;
		if (sample->is_fg)
			state->window_fg_complete_count++;
	}
	state->window_mean_dev_us = div_u64(latency_sum,
					    SMART_IO_DEV_LAT_WINDOW);
	return true;
}

static bool smart_io_throttle_snapshot_state_locked(
		struct smart_io_throttle_ctx *ctx,
		struct smart_io_state_snapshot *state, u64 now_ns, u32 queue_max,
		u32 threshold_us)
{
	struct smart_io_rq_meta *meta;
	u32 issued_seen = 0;
	u32 waiting_seen = 0;
	int bkt;

	memset(state, 0, sizeof(*state));
	state->decision_ts_ns = now_ns;
	state->queue_max = queue_max;
	if (!queue_max ||
	    !smart_io_throttle_window_snapshot_locked(ctx, state, threshold_us))
		return false;

	hash_for_each(ctx->active, bkt, meta, active_node) {
		if (meta->issued_accounted) {
			issued_seen++;
			if (meta->class == SMART_IO_RQ_FOREGROUND)
				state->device_q_fg_count++;
			if (meta->issue_ts_ns && now_ns >= meta->issue_ts_ns &&
			    now_ns - meta->issue_ts_ns >=
				    (u64)threshold_us * NSEC_PER_USEC)
				state->device_q_slow_count++;
		}
		if (!meta->queued)
			continue;
		waiting_seen++;
		if (meta->class == SMART_IO_RQ_BACKGROUND)
			state->waiting_count++;
		else if (meta->class == SMART_IO_RQ_FOREGROUND)
			state->waiting_fg++;
		if (meta->high_ioprio)
			state->waiting_high_ioprio_count++;
		if (smart_io_throttle_contiguous_locked(ctx, meta))
			state->waiting_to_issued_contiguous_count++;
	}

	state->device_q_count = ctx->issued_depth;
	state->reserved_depth_count = ctx->current_depth;
	state->waiting_total_count = waiting_seen;
	if (issued_seen != ctx->issued_depth ||
	    waiting_seen != ctx->pending_special + ctx->pending_fg +
			    ctx->pending_bg) {
		ctx->depth_anomalies++;
		return false;
	}
	return true;
}

static bool smart_io_throttle_slow_window_locked(
		struct smart_io_throttle_ctx *ctx, u32 threshold_us)
{
	struct smart_io_state_snapshot state = { };

	return smart_io_throttle_window_snapshot_locked(ctx, &state,
						 threshold_us) &&
		state.window_slow_count == SMART_IO_DEV_LAT_WINDOW;
}

void smart_io_throttle_record_issue(struct request *rq)
{
	struct smart_io_throttle_ctx *ctx;
	struct smart_io_rq_meta *meta;
	struct smart_io_rq_meta trace_meta;
	struct smart_io_action feedback_action;
	unsigned long flags;
	u64 now_ns;
	u32 feedback_sample_no = 0;
	u32 feedback_fast_votes = 0;
	u32 feedback_slow_votes = 0;
	bool trace_feedback = false;
	bool wake = false;

	if (!rq || !rq->q)
		return;

	rcu_read_lock();
	ctx = smart_io_throttle_find_ctx_rcu(rq->q);
	if (!ctx)
		goto unlock_rcu;

	now_ns = ktime_get_boottime_ns();
	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_find_meta_locked(ctx, rq);
	if (!meta)
		meta = smart_io_throttle_alloc_meta_locked(ctx, rq, SMART_IO_RQ_SPECIAL);
	if (!meta) {
		wake = true;
		spin_unlock_irqrestore(&ctx->lock, flags);
		goto run_queues;
	}

	if (!meta->depth_accounted)
		wake = smart_io_throttle_account_depth_locked(ctx,
							      &meta->depth_accounted);
	if (!meta->issued_accounted)
		wake |= smart_io_throttle_account_issued_depth_locked(ctx,
								 &meta->issued_accounted);
	meta->op = req_op(rq);
	smart_io_throttle_inflight_add_locked(ctx, meta, rq);
	if (!meta->issue_ts_ns) {
		meta->issue_ts_ns = now_ns;
		meta->completion_sampled = false;
		meta->issue_session_id = smart_io_throttle_enabled() ?
			ctx->session_id : 0;
	}
	if (ctx->feedback_meta == meta &&
	    ctx->feedback_state == SMART_IO_FEEDBACK_WAIT_ISSUE &&
	    meta->dispatch_decision_id == ctx->action.decision_id) {
		ctx->feedback_issue_ts_ns = meta->issue_ts_ns;
		ctx->feedback_state = SMART_IO_FEEDBACK_WAIT_COMPLETE;
		trace_meta = *meta;
		feedback_action = ctx->action;
		feedback_sample_no = ctx->feedback_vote_count + 1;
		feedback_fast_votes = ctx->feedback_fast_votes;
		feedback_slow_votes = ctx->feedback_slow_votes;
		trace_feedback = true;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (trace_feedback)
		smart_io_throttle_trace_feedback(ctx, &trace_meta, &feedback_action,
						 "issued", trace_meta.issue_ts_ns,
						 0, 0,
						 BLK_STS_OK, 0,
						 feedback_sample_no,
						 feedback_fast_votes,
						 feedback_slow_votes);
run_queues:
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
unlock_rcu:
	rcu_read_unlock();
}

void smart_io_throttle_record_requeue(struct request *rq)
{
	struct smart_io_throttle_ctx *ctx;
	struct smart_io_rq_meta *meta;
	struct smart_io_rq_meta feedback_meta;
	struct smart_io_action feedback_action;
	enum smart_io_gate_reason released_reason = SMART_IO_GATE_NONE;
	unsigned long flags;
	const char *feedback_result = NULL;
	u32 current_depth = 0;
	u32 target_depth = 0;
	u32 feedback_sample_no = 0;
	u32 feedback_fast_votes = 0;
	u32 feedback_slow_votes = 0;
	bool trace_feedback = false;
	bool wake = false;

	if (!rq || !rq->q)
		return;

	rcu_read_lock();
	ctx = smart_io_throttle_find_ctx_rcu(rq->q);
	if (!ctx)
		goto unlock_rcu;

	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_find_meta_locked(ctx, rq);
	if (!meta) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		goto unlock_rcu;
	}
	if (ctx->feedback_meta == meta) {
		feedback_sample_no = ctx->feedback_vote_count + 1;
		feedback_fast_votes = ctx->feedback_fast_votes;
		feedback_slow_votes = ctx->feedback_slow_votes;
	}
	smart_io_throttle_invalidate_feedback_locked(ctx, meta, &feedback_result);
	if (feedback_result) {
		feedback_meta = *meta;
		feedback_action = ctx->action;
		trace_feedback = true;
	}
	smart_io_throttle_inflight_remove_locked(meta);
	wake |= smart_io_throttle_release_deadline_locked(ctx, meta);
	wake |= smart_io_throttle_release_issued_depth_locked(ctx,
								    &meta->issued_accounted);
	wake |= smart_io_throttle_release_depth_locked(ctx,
						       &meta->depth_accounted,
						       &released_reason);
	meta->issue_ts_ns = 0;
	meta->issue_session_id = 0;
	meta->dispatch_decision_id = 0;
	meta->completion_sampled = false;
	current_depth = ctx->current_depth;
	target_depth = ctx->action.target_depth;
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (trace_feedback)
		smart_io_throttle_trace_feedback(ctx, &feedback_meta,
						 &feedback_action, feedback_result,
						 0, 0, 0, BLK_STS_OK, 0,
						 feedback_sample_no,
						 feedback_fast_votes,
						 feedback_slow_votes);
	if (released_reason != SMART_IO_GATE_NONE)
		smart_io_throttle_trace_gate(ctx, "release", released_reason,
					     current_depth, target_depth);
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
unlock_rcu:
	rcu_read_unlock();
}

void smart_io_throttle_record_complete(struct request *rq, blk_status_t status,
				       unsigned int nr_bytes)
{
	struct smart_io_throttle_ctx *ctx;
	struct smart_io_rq_meta *meta;
	struct smart_io_rq_meta feedback_meta;
	struct smart_io_action action;
	struct smart_io_action feedback_action;
	enum smart_io_gate_reason released_reason = SMART_IO_GATE_NONE;
	unsigned long flags;
	u64 now_ns;
	u64 delta_us;
	u64 session_id = 0;
	u64 control_id = 0;
	u64 decision_id = 0;
	u32 latency_us = 0;
	u32 threshold_us;
	u32 current_depth = 0;
	u32 target_depth = 0;
	u32 feedback_sample_no = 0;
	u32 feedback_fast_votes = 0;
	u32 feedback_slow_votes = 0;
	const char *feedback_result = NULL;
	bool final;
	bool start_inference = false;
	bool slow_window_trigger = false;
	bool reinfer_requested = false;
	bool feedback_complete = false;
	bool latency_valid = false;
	bool trace_feedback = false;
	bool wake = false;

	if (!rq || !rq->q)
		return;
	final = !rq->bio || nr_bytes >= blk_rq_bytes(rq);

	rcu_read_lock();
	ctx = smart_io_throttle_find_ctx_rcu(rq->q);
	if (!ctx)
		goto unlock_rcu;

	now_ns = ktime_get_boottime_ns();
	threshold_us = smart_io_throttle_get_dev_lat_threshold();
	spin_lock_irqsave(&ctx->lock, flags);
	meta = smart_io_throttle_find_meta_locked(ctx, rq);
	if (!meta) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		goto unlock_rcu;
	}

	if (meta->issue_ts_ns && now_ns > meta->issue_ts_ns) {
		delta_us = div_u64(now_ns - meta->issue_ts_ns, NSEC_PER_USEC);
		latency_us = min_t(u64, delta_us, U32_MAX);
		latency_valid = true;
	}
	if (final && latency_valid && !meta->completion_sampled) {
		meta->completion_sampled = true;
		if (smart_io_throttle_enabled() &&
		    meta->issue_session_id == ctx->session_id) {
			ctx->dev_lat[ctx->dev_lat_head].complete_ts_ns = now_ns;
			ctx->dev_lat[ctx->dev_lat_head].dev_lat_us = latency_us;
			ctx->dev_lat[ctx->dev_lat_head].is_fg =
				meta->class == SMART_IO_RQ_FOREGROUND;
			ctx->dev_lat_head = (ctx->dev_lat_head + 1) %
				SMART_IO_DEV_LAT_WINDOW;
			if (ctx->dev_lat_count < SMART_IO_DEV_LAT_WINDOW)
				ctx->dev_lat_count++;

			if (!ctx->throttle_active && !ctx->inference_pending &&
			    smart_io_throttle_slow_window_locked(ctx, threshold_us))
				slow_window_trigger = true;
		}
	}

	if (final && latency_valid && ctx->feedback_meta == meta &&
	    ctx->feedback_state == SMART_IO_FEEDBACK_WAIT_COMPLETE &&
	    ctx->feedback_issue_ts_ns) {
		feedback_complete = true;
		trace_feedback = true;
		feedback_meta = *meta;
		feedback_action = ctx->action;
		smart_io_throttle_clear_feedback_binding_locked(ctx);
		ctx->feedback_vote_count++;
		if (latency_us < threshold_us)
			ctx->feedback_fast_votes++;
		else
			ctx->feedback_slow_votes++;
		feedback_sample_no = ctx->feedback_vote_count;
		feedback_fast_votes = ctx->feedback_fast_votes;
		feedback_slow_votes = ctx->feedback_slow_votes;

		if (ctx->feedback_fast_votes >=
		    SMART_IO_FEEDBACK_VOTES_REQUIRED) {
			feedback_result = "recovered";
			wake = smart_io_throttle_fallback_locked(ctx);
		} else if (ctx->feedback_slow_votes >=
			   SMART_IO_FEEDBACK_VOTES_REQUIRED) {
			feedback_result = "slow_reinfer";
			reinfer_requested = true;
		} else {
			feedback_result = latency_us < threshold_us ?
				"fast_vote" : "slow_vote";
			ctx->feedback_state = SMART_IO_FEEDBACK_WAIT_DISPATCH;
		}
	}
	if (!feedback_complete && final && ctx->feedback_meta == meta) {
		feedback_sample_no = ctx->feedback_vote_count + 1;
		feedback_fast_votes = ctx->feedback_fast_votes;
		feedback_slow_votes = ctx->feedback_slow_votes;
		feedback_meta = *meta;
		feedback_action = ctx->action;
		smart_io_throttle_invalidate_feedback_locked(ctx, meta,
							     &feedback_result);
		trace_feedback = feedback_result != NULL;
	}
	if (final) {
		smart_io_throttle_inflight_remove_locked(meta);
		wake |= smart_io_throttle_release_deadline_locked(ctx, meta);
		wake |= smart_io_throttle_release_issued_depth_locked(ctx,
								    &meta->issued_accounted);
		wake |= smart_io_throttle_release_depth_locked(ctx,
							       &meta->depth_accounted,
							       &released_reason);
		smart_io_throttle_free_meta_locked(ctx, rq, meta);
	}
	if (reinfer_requested || slow_window_trigger) {
		start_inference = smart_io_throttle_start_inference_locked(
			ctx, slow_window_trigger);
		if (start_inference) {
			session_id = ctx->pending_session_id;
			control_id = ctx->pending_control_id;
			decision_id = ctx->pending_decision_id;
			action = ctx->pending_fixed_action;
		} else if (reinfer_requested) {
			wake |= smart_io_throttle_fallback_locked(ctx);
		}
	}
	current_depth = ctx->current_depth;
	target_depth = ctx->action.target_depth;
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (trace_feedback)
		smart_io_throttle_trace_feedback(ctx, &feedback_meta,
						 &feedback_action,
						 feedback_result,
						 feedback_meta.issue_ts_ns, now_ns,
						 latency_us,
						 status, nr_bytes,
						 feedback_sample_no,
						 feedback_fast_votes,
						 feedback_slow_votes);
	if (start_inference) {
		smart_io_throttle_trace_decision(ctx,
					 feedback_complete ? "feedback_slow" : "slow_window",
					 "queued", session_id, control_id, decision_id,
					 &action);
		smart_io_throttle_schedule_inference(ctx);
	}
	if (released_reason != SMART_IO_GATE_NONE)
		smart_io_throttle_trace_gate(ctx, "release", released_reason,
					     current_depth, target_depth);
	if (wake)
		blk_mq_run_hw_queues(ctx->queue, true);
unlock_rcu:
	rcu_read_unlock();
}

static void smart_io_throttle_reset_context(struct smart_io_throttle_ctx *ctx,
					     bool new_session)
{
	struct smart_io_rq_meta *meta;
	unsigned long flags;
	u64 now_ns = new_session ? ktime_get_boottime_ns() : 0;
	int bkt;

	spin_lock_irqsave(&ctx->lock, flags);
	if (new_session)
		ctx->session_id++;
	smart_io_throttle_fallback_locked(ctx);
	hash_for_each(ctx->active, bkt, meta, active_node) {
		if (new_session && meta->queued &&
		    meta->class == SMART_IO_RQ_BACKGROUND)
			meta->queued_ts_ns = now_ns;
		else
			meta->queued_ts_ns = 0;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	hrtimer_cancel(&ctx->inference_timer);
	cancel_work_sync(&ctx->inference_work);
	cancel_work_sync(&ctx->timeout_work);
	blk_mq_run_hw_queues(ctx->queue, true);
}

int smart_io_throttle_set_enabled(bool enabled)
{
	struct smart_io_throttle_ctx *ctx;
	bool was_enabled = smart_io_throttle_enabled();

	if (enabled == was_enabled)
		return 0;

	atomic_set(&throttle_enabled, 0);
	mutex_lock(&active_throttle_queues_lock);
	list_for_each_entry(ctx, &active_throttle_queues, active_node)
		smart_io_throttle_reset_context(ctx, enabled);
	mutex_unlock(&active_throttle_queues_lock);
	if (enabled)
		atomic_set(&throttle_enabled, 1);

	return 0;
}

bool smart_io_throttle_get_enabled(void)
{
	return smart_io_throttle_enabled();
}

int smart_io_throttle_set_dev_lat_threshold(u32 threshold_us)
{
	if (!threshold_us || threshold_us > INT_MAX)
		return -EINVAL;
	if (smart_io_throttle_enabled())
		return -EBUSY;

	atomic_set(&dev_lat_threshold_us, threshold_us);
	return 0;
}

u32 smart_io_throttle_get_dev_lat_threshold(void)
{
	return (u32)atomic_read(&dev_lat_threshold_us);
}

int smart_io_throttle_set_background_deadline_ms(u32 deadline_ms)
{
	if (!deadline_ms || deadline_ms > INT_MAX)
		return -EINVAL;
	if (smart_io_throttle_enabled())
		return -EBUSY;

	atomic_set(&background_deadline_ms, deadline_ms);
	return 0;
}

u32 smart_io_throttle_get_background_deadline_ms(void)
{
	return (u32)atomic_read(&background_deadline_ms);
}

int smart_io_throttle_set_ratios(u32 light_pct, u32 medium_pct,
				 u32 heavy_pct)
{
	unsigned long flags;

	if (light_pct > 100 || !light_pct || light_pct <= medium_pct ||
	    !medium_pct || medium_pct <= heavy_pct || !heavy_pct)
		return -EINVAL;

	spin_lock_irqsave(&throttle_config_lock, flags);
	throttle_config.light_pct = light_pct;
	throttle_config.medium_pct = medium_pct;
	throttle_config.heavy_pct = heavy_pct;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
	return 0;
}

void smart_io_throttle_get_ratios(u32 *light_pct, u32 *medium_pct,
				  u32 *heavy_pct)
{
	unsigned long flags;

	spin_lock_irqsave(&throttle_config_lock, flags);
	*light_pct = throttle_config.light_pct;
	*medium_pct = throttle_config.medium_pct;
	*heavy_pct = throttle_config.heavy_pct;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
}

int smart_io_throttle_set_action_source(enum smart_io_action_source source)
{
	unsigned long flags;

	if (source > SMART_IO_ACTION_SOURCE_MONOTONIC)
		return -EINVAL;

	spin_lock_irqsave(&throttle_config_lock, flags);
	throttle_config.source = source;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
	return 0;
}

enum smart_io_action_source smart_io_throttle_get_action_source(void)
{
	enum smart_io_action_source source;
	unsigned long flags;

	spin_lock_irqsave(&throttle_config_lock, flags);
	source = throttle_config.source;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
	return source;
}

int smart_io_throttle_set_fixed_action(enum smart_io_throttle_level level,
					 enum smart_io_dispatch_policy policy)
{
	unsigned long flags;

	if (level >= SMART_IO_THROTTLE_LEVEL_COUNT ||
	    policy >= SMART_IO_DISPATCH_POLICY_COUNT)
		return -EINVAL;

	spin_lock_irqsave(&throttle_config_lock, flags);
	throttle_config.fixed_action.level = level;
	throttle_config.fixed_action.policy = policy;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
	return 0;
}

void smart_io_throttle_get_fixed_action(enum smart_io_throttle_level *level,
					  enum smart_io_dispatch_policy *policy)
{
	unsigned long flags;

	spin_lock_irqsave(&throttle_config_lock, flags);
	*level = throttle_config.fixed_action.level;
	*policy = throttle_config.fixed_action.policy;
	spin_unlock_irqrestore(&throttle_config_lock, flags);
}

const char *smart_io_throttle_level_name(enum smart_io_throttle_level level)
{
	if (level >= SMART_IO_THROTTLE_LEVEL_COUNT)
		return "INVALID";
	return throttle_level_names[level];
}

const char *smart_io_dispatch_policy_name(enum smart_io_dispatch_policy policy)
{
	if (policy >= SMART_IO_DISPATCH_POLICY_COUNT)
		return "INVALID";
	return dispatch_policy_names[policy];
}

const char *smart_io_action_source_name(enum smart_io_action_source source)
{
	if (source > SMART_IO_ACTION_SOURCE_MONOTONIC)
		return "invalid";
	return action_source_names[source];
}

const char *smart_io_gate_reason_name(enum smart_io_gate_reason reason)
{
	if (reason > SMART_IO_GATE_DEPTH)
		return "invalid";
	return gate_reason_names[reason];
}

const char *smart_io_feedback_state_name(enum smart_io_feedback_state state)
{
	if (state > SMART_IO_FEEDBACK_WAIT_COMPLETE)
		return "invalid";
	return feedback_state_names[state];
}

void smart_io_throttle_get_stats(struct smart_io_throttle_stats *stats)
{
	struct smart_io_throttle_ctx *ctx;
	unsigned long flags;
	u32 light_pct;
	u32 medium_pct;
	u32 heavy_pct;

	memset(stats, 0, sizeof(*stats));
	stats->enabled = smart_io_throttle_enabled();
	stats->action_source = smart_io_throttle_get_action_source();
	stats->dev_lat_threshold_us = smart_io_throttle_get_dev_lat_threshold();
	stats->background_deadline_ms =
		smart_io_throttle_get_background_deadline_ms();
	smart_io_throttle_get_ratios(&light_pct, &medium_pct, &heavy_pct);
	stats->light_pct = light_pct;
	stats->medium_pct = medium_pct;
	stats->heavy_pct = heavy_pct;

	mutex_lock(&active_throttle_queues_lock);
	list_for_each_entry(ctx, &active_throttle_queues, active_node) {
		spin_lock_irqsave(&ctx->lock, flags);
		stats->active_queues++;
		stats->active |= ctx->throttle_active;
		stats->inference_pending |= ctx->inference_pending;
		stats->action_valid |= ctx->action_valid;
		stats->level = ctx->action.level;
		stats->policy = ctx->action.policy;
		stats->gate_reason = ctx->gate_reason;
		stats->feedback_state = ctx->feedback_state;
		stats->resolved_ratio_pct = ctx->action.ratio_pct;
		stats->queue_max = ctx->action.queue_max;
		stats->target_depth = ctx->action.target_depth;
		stats->current_depth += ctx->current_depth;
		stats->issued_depth += ctx->issued_depth;
		stats->meta_capacity += ctx->meta_capacity;
		stats->meta_inuse += ctx->meta_inuse;
		stats->special_pending += ctx->pending_special;
		stats->fg_pending += ctx->pending_fg;
		stats->bg_pending += ctx->pending_bg;
		stats->deadline_forced_active |= ctx->deadline_forced_active;
		stats->session_id = ctx->session_id;
		stats->control_id = ctx->control_id;
		stats->decision_id = ctx->decision_id;
		stats->feedback_rq_id = ctx->feedback_meta ?
			ctx->feedback_meta->rq_id : 0;
		stats->feedback_vote_count += ctx->feedback_vote_count;
		stats->feedback_fast_votes += ctx->feedback_fast_votes;
		stats->feedback_slow_votes += ctx->feedback_slow_votes;
		stats->inference_count += ctx->inference_count;
		stats->timeout_count += ctx->timeout_count;
		stats->invalid_result_count += ctx->invalid_result_count;
		stats->special_dispatched += ctx->special_dispatched;
		stats->fg_dispatched += ctx->fg_dispatched;
		stats->bg_dispatched += ctx->bg_dispatched;
		stats->bg_blocked += ctx->bg_blocked;
		stats->bg_deadline_forced += ctx->bg_deadline_forced;
		stats->baseline_hits += ctx->baseline_hits;
		stats->seq_hits += ctx->seq_hits;
		stats->seq_fallbacks += ctx->seq_fallbacks;
		stats->small_hits += ctx->small_hits;
		stats->depth_anomalies += ctx->depth_anomalies;
		stats->feedback_rebind_count += ctx->feedback_rebind_count;
		stats->allocation_failures += ctx->allocation_failures;
		spin_unlock_irqrestore(&ctx->lock, flags);
	}
	mutex_unlock(&active_throttle_queues_lock);
}
