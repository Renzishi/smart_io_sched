/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMART_IO_THROTTLE_H
#define SMART_IO_THROTTLE_H

#include <linux/blk-mq.h>
#include <linux/completion.h>
#include <linux/hashtable.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define SMART_IO_DEV_LAT_WINDOW 5
#define SMART_IO_DEV_LAT_WINDOW_MAX_US 10000U
#define SMART_IO_FEEDBACK_SAMPLE_MAX 5
#define SMART_IO_ACTIVE_HASH_BITS 6
#define SMART_IO_INFLIGHT_HASH_BITS 6
enum smart_io_rq_class {
	SMART_IO_RQ_FOREGROUND,
	SMART_IO_RQ_BACKGROUND,
};

enum smart_io_queue_class {
	SMART_IO_QUEUE_HP,
	SMART_IO_QUEUE_RT,
	SMART_IO_QUEUE_BE,
	SMART_IO_QUEUE_COUNT,
};

enum smart_io_throttle_level {
	SMART_IO_THROTTLE_NO,
	SMART_IO_THROTTLE_LIGHT,
	SMART_IO_THROTTLE_MEDIUM,
	SMART_IO_THROTTLE_HEAVY,
	SMART_IO_THROTTLE_LEVEL_COUNT,
};

enum smart_io_dispatch_policy {
	SMART_IO_DISPATCH_BASELINE,
	SMART_IO_DISPATCH_SEQ,
	SMART_IO_DISPATCH_SMALL,
	SMART_IO_DISPATCH_POLICY_COUNT,
};

enum smart_io_action_source {
	SMART_IO_ACTION_SOURCE_FIXED,
	SMART_IO_ACTION_SOURCE_MODEL,
	SMART_IO_ACTION_SOURCE_COUNT,
};

enum smart_io_feedback_state {
	SMART_IO_FEEDBACK_NONE,
	SMART_IO_FEEDBACK_WAIT_DISPATCH,
	SMART_IO_FEEDBACK_WAIT_ISSUE,
	SMART_IO_FEEDBACK_WAIT_COMPLETE,
};

enum smart_io_gate_reason {
	SMART_IO_GATE_NONE,
	SMART_IO_GATE_INFERENCE,
	SMART_IO_GATE_DEPTH,
};

enum smart_io_dispatch_selection {
	SMART_IO_SELECTION_FOREGROUND_FIFO,
	SMART_IO_SELECTION_SEQ_HIT,
	SMART_IO_SELECTION_SEQ_FALLBACK,
	SMART_IO_SELECTION_SMALL,
	SMART_IO_SELECTION_BACKGROUND_BASELINE,
	SMART_IO_SELECTION_BACKGROUND_DEADLINE,
};

struct smart_io_action {
	enum smart_io_throttle_level level;
	enum smart_io_dispatch_policy policy;
	u32 ratio_pct;
	u32 queue_max;
	u32 target_depth;
	u64 decision_id;
};

struct smart_io_completion_sample {
	u64 complete_ts_ns;
	u32 dev_lat_us;
	bool is_fg;
};

struct smart_io_state_snapshot {
	u64 decision_ts_ns;
	u32 device_q_count;
	u32 reserved_depth_count;
	u32 device_q_fg_count;
	u32 device_q_slow_count;
	u32 queue_max;
	u32 dev_lat_threshold_us;
	u32 window_complete_count;
	u32 window_mean_dev_us;
	u32 window_span_us;
	u32 window_slow_count;
	u32 window_fg_complete_count;
	u32 waiting_total_count;
	u32 waiting_count;
	u32 waiting_fg;
	u32 waiting_high_ioprio_count;
	u32 waiting_to_issued_contiguous_count;
};

struct smart_io_rq_meta {
	struct list_head free_node;
	struct hlist_node active_node;
	struct hlist_node inflight_node;
	struct hlist_node inflight_start_node;
	struct request *rq;
	enum smart_io_rq_class class;
	enum smart_io_queue_class queue_class;
	enum req_op op;
	sector_t start_sector;
	sector_t end_sector;
	u64 rq_id;
	u64 issue_ts_ns;
	u64 issue_session_id;
	u64 dispatch_decision_id;
	u64 queued_ts_ns;
	u32 tag_depth_max;
	bool queued;
	bool depth_accounted; /* current_depth reservation */
	bool bg_depth_accounted; /* bg_current_depth reservation */
	bool issued_accounted; /* issued_depth in-flight */
	bool inflight_indexed;
	bool feedback_bound;
	bool completion_sampled;
	bool deadline_forced;
	bool high_ioprio;
	bool device_time_excluded;
};

struct smart_io_throttle_ctx {
	struct request_queue *queue;
	struct list_head active_node;
	spinlock_t lock;
	struct list_head free_meta;
	struct smart_io_rq_meta *meta_pool;
	u32 meta_capacity;
	u32 meta_inuse;
	DECLARE_HASHTABLE(active, SMART_IO_ACTIVE_HASH_BITS);
	DECLARE_HASHTABLE(inflight, SMART_IO_INFLIGHT_HASH_BITS);
	DECLARE_HASHTABLE(inflight_start, SMART_IO_INFLIGHT_HASH_BITS);

	bool dying;
	bool throttle_active;
	bool inference_pending;
	bool action_valid;
	bool gate_blocked;
	bool deadline_forced_active;
	enum smart_io_gate_reason gate_reason;
	enum smart_io_feedback_state feedback_state;

	u32 current_depth; /* dispatch to requeue/final complete */
	u32 bg_current_depth; /* background dispatch to final complete */
	u32 issued_depth; /* issue to requeue/final complete */
	u32 pending_fg;
	u32 pending_bg;
	struct smart_io_completion_sample dev_lat[SMART_IO_DEV_LAT_WINDOW];
	u8 dev_lat_head;
	u8 dev_lat_count;

	u64 session_id;
	u64 control_id;
	u64 decision_id;
	u64 next_rq_id;
	struct smart_io_action action;
	struct smart_io_rq_meta *feedback_meta;
	u64 feedback_dispatch_ts_ns;
	u64 feedback_issue_ts_ns;
	u8 feedback_vote_count;
	u64 feedback_total_dev_lat_us;
	u32 feedback_max_dev_lat_us;

	u64 pending_session_id;
	u64 pending_control_id;
	u64 pending_decision_id;
	enum smart_io_action_source pending_source;
	struct smart_io_action pending_fixed_action;
	struct smart_io_state_snapshot pending_state;
	bool pending_state_valid;
	u32 pending_light_pct;
	u32 pending_medium_pct;
	u32 pending_heavy_pct;
	bool inference_timed_out;
	bool inference_running;
	struct completion model_thread_idle;
	u64 model_trigger_ts_ns;
	u64 model_submit_ts_ns;
	u64 model_thread_start_ts_ns;
	u64 model_state_ts_ns;
	u64 model_predict_start_ts_ns;
	u64 model_predict_end_ts_ns;
	u64 model_action_apply_ts_ns;
	bool model_post_apply_pending;

	struct workqueue_struct *inference_wq;
	struct workqueue_struct *timeout_wq;
	struct work_struct inference_work;
	struct work_struct timeout_work;
	struct hrtimer inference_timer;

	u64 inference_count;
	u64 timeout_count;
	u64 invalid_result_count;
	u64 fg_dispatched;
	u64 bg_dispatched;
	u64 bg_blocked;
	u64 bg_deadline_forced;
	u64 baseline_hits;
	u64 seq_hits;
	u64 seq_fallbacks;
	u64 small_hits;
	u64 depth_anomalies;
	u64 feedback_rebind_count;
	u64 allocation_failures;
};

struct smart_io_throttle_stats {
	bool enabled;
	bool active;
	bool inference_pending;
	bool action_valid;
	u32 active_queues;
	enum smart_io_action_source action_source;
	enum smart_io_throttle_level level;
	enum smart_io_dispatch_policy policy;
	enum smart_io_gate_reason gate_reason;
	enum smart_io_feedback_state feedback_state;
	u32 light_pct;
	u32 medium_pct;
	u32 heavy_pct;
	u32 resolved_ratio_pct;
	u32 queue_max;
	u32 target_depth;
	u32 current_depth;
	u32 bg_current_depth;
	u32 issued_depth;
	u32 meta_capacity;
	u32 meta_inuse;
	u32 fg_pending;
	u32 bg_pending;
	u32 dev_lat_threshold_us;
	u32 background_deadline_ms;
	bool deadline_forced_active;
	u64 session_id;
	u64 control_id;
	u64 decision_id;
	u64 feedback_rq_id;
	u32 feedback_vote_count;
	u32 feedback_mean_dev_lat_us;
	u32 feedback_max_dev_lat_us;
	u64 inference_count;
	u64 timeout_count;
	u64 invalid_result_count;
	u64 fg_dispatched;
	u64 bg_dispatched;
	u64 bg_blocked;
	u64 bg_deadline_forced;
	u64 baseline_hits;
	u64 seq_hits;
	u64 seq_fallbacks;
	u64 small_hits;
	u64 depth_anomalies;
	u64 feedback_rebind_count;
	u64 allocation_failures;
};

int smart_io_throttle_queue_init(struct smart_io_throttle_ctx *ctx,
				 struct request_queue *q);
void smart_io_throttle_queue_exit(struct smart_io_throttle_ctx *ctx);
void smart_io_throttle_model_thread_exit(void);

void smart_io_throttle_prepare_request(struct request *rq);
void smart_io_throttle_classify_request(struct smart_io_throttle_ctx *ctx,
					struct request *rq);
void smart_io_throttle_update_request(struct smart_io_throttle_ctx *ctx,
				      struct request *rq);
void smart_io_throttle_unqueue_request(struct smart_io_throttle_ctx *ctx,
					struct request *rq);
void smart_io_throttle_account_dispatch(struct smart_io_throttle_ctx *ctx,
					struct request *rq,
					enum smart_io_dispatch_selection selection);
void smart_io_throttle_finish_request(struct smart_io_throttle_ctx *ctx,
					struct request *rq);

enum smart_io_rq_class smart_io_throttle_rq_class(const struct request *rq);
enum smart_io_queue_class
smart_io_throttle_select_queue(const struct request *rq);
enum smart_io_queue_class
smart_io_throttle_select_bio_queue(const struct bio *bio);
void smart_io_throttle_mark_bio_hp(struct bio *bio);
void smart_io_throttle_mark_request_hp(struct request *rq);
int smart_io_throttle_set_ux_hp_enable(bool enabled);
bool smart_io_throttle_get_ux_hp_enable(void);
int smart_io_throttle_set_rt_read_hp_enable(bool enabled);
bool smart_io_throttle_get_rt_read_hp_enable(void);
bool smart_io_throttle_enabled(void);
int smart_io_throttle_set_queue_rq_demo(bool enabled);
bool smart_io_throttle_get_queue_rq_demo(void);
bool smart_io_throttle_ufs_should_requeue(struct request *rq);
bool smart_io_throttle_has_pending(struct smart_io_throttle_ctx *ctx,
				   enum smart_io_rq_class class);
void smart_io_throttle_dispatch_model_if_needed(
		struct smart_io_throttle_ctx *ctx);
bool smart_io_throttle_model_dispatch_needed(
		struct smart_io_throttle_ctx *ctx);
bool smart_io_throttle_non_hp_allowed(struct smart_io_throttle_ctx *ctx);
bool smart_io_throttle_deadline_escape_available(
		struct smart_io_throttle_ctx *ctx);
bool smart_io_throttle_background_deadline_expired(
		struct smart_io_throttle_ctx *ctx, const struct request *rq,
		u64 now_ns, u64 *queued_ts_ns);
enum smart_io_dispatch_policy
smart_io_throttle_effective_policy(struct smart_io_throttle_ctx *ctx);
bool smart_io_throttle_seq_match(struct smart_io_throttle_ctx *ctx,
				  const struct request *rq);
u32 smart_io_throttle_scan_budget(const struct smart_io_throttle_ctx *ctx);

void smart_io_throttle_record_issue(struct request *rq);
void smart_io_throttle_record_requeue(struct request *rq);
void smart_io_throttle_record_complete(struct request *rq, blk_status_t status,
				       unsigned int nr_bytes);

int smart_io_throttle_set_enabled(bool enabled);
bool smart_io_throttle_get_enabled(void);
int smart_io_throttle_set_dev_lat_threshold(u32 threshold_us);
u32 smart_io_throttle_get_dev_lat_threshold(void);
int smart_io_throttle_set_background_deadline_ms(u32 deadline_ms);
u32 smart_io_throttle_get_background_deadline_ms(void);
int smart_io_throttle_set_ratios(u32 light_pct, u32 medium_pct,
				 u32 heavy_pct);
void smart_io_throttle_get_ratios(u32 *light_pct, u32 *medium_pct,
				  u32 *heavy_pct);
int smart_io_throttle_set_action_source(enum smart_io_action_source source);
enum smart_io_action_source smart_io_throttle_get_action_source(void);
int smart_io_throttle_set_fixed_action(enum smart_io_throttle_level level,
					 enum smart_io_dispatch_policy policy);
void smart_io_throttle_get_fixed_action(enum smart_io_throttle_level *level,
					  enum smart_io_dispatch_policy *policy);
const char *smart_io_throttle_level_name(enum smart_io_throttle_level level);
const char *smart_io_dispatch_policy_name(enum smart_io_dispatch_policy policy);
const char *smart_io_action_source_name(enum smart_io_action_source source);
const char *smart_io_gate_reason_name(enum smart_io_gate_reason reason);
const char *smart_io_feedback_state_name(enum smart_io_feedback_state state);
void smart_io_throttle_get_stats(struct smart_io_throttle_stats *stats);

#endif
