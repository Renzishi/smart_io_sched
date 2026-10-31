// SPDX-License-Identifier: GPL-2.0
/*
 *  MQ Deadline i/o scheduler - adaptation of the legacy deadline scheduler,
 *  for the blk-mq scheduling framework
 *
 *  Copyright (C) 2016 Jens Axboe <axboe@kernel.dk>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/ktime.h>
#include <linux/preempt.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>

#include <trace/events/block.h>

#include <block/elevator.h>
#include <block/blk.h>
#include <block/blk-mq.h>
#include <block/blk-mq-sched.h>

#include "smart_deadline.h"
#include "smart_io_rl.h"
#include "smart_io_throttle.h"
#include "smart_io_prio_policy.h"

/*
 * See Documentation/block/deadline-iosched.rst
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
/*
 * Time after which to dispatch lower priority requests even if higher
 * priority requests are pending.
 */
static const int prio_aging_expire = 10 * HZ;
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

/*
 * VIP/HP(前台同步读)插入后同步跑一次 hw queue：把 blk-mq 给 elevator 队列安排的
 * 异步派发(kblockd)变成本任务内的内联派发。默认关闭，便于 A/B。
 * hp_inline_dispatch 打开时覆盖 VIP 与 HP 两类读请求。
 */
static bool vip_inline_dispatch;
module_param(vip_inline_dispatch, bool, 0644);
MODULE_PARM_DESC(vip_inline_dispatch,
		 "Run the hw queue synchronously after inserting a VIP request");

static bool hp_inline_dispatch;
module_param(hp_inline_dispatch, bool, 0644);
MODULE_PARM_DESC(hp_inline_dispatch,
		 "Run the hw queue synchronously after inserting an HP/VIP read request");

/*
 * 让 insert-merge 命中“前一条插入的请求”：内核 elv_attempt_insert_merge()
 * 只检查 q->last_merge（通常指向队头）和按起始扇区的哈希，深队列 FIFO 下
 * 相邻 4K 永远合不上。打开后每次插入前把 q->last_merge 指向前一条。
 */
static bool merge_last_insert;
module_param(merge_last_insert, bool, 0644);
MODULE_PARM_DESC(merge_last_insert,
		 "Point q->last_merge at the most recently inserted request to enable back merges");

static bool dd_should_inline_dispatch(const struct request *rq)
{
	u16 ioprio;

	if (!vip_inline_dispatch && !hp_inline_dispatch)
		return false;
	if (req_op((struct request *)rq) != REQ_OP_READ)
		return false;
	ioprio = req_get_ioprio((struct request *)rq);
	if (vip_inline_dispatch && is_vip_ioprio(ioprio))
		return true;
	if (hp_inline_dispatch &&
	    (is_hp_ioprio(ioprio) || is_vip_ioprio(ioprio)))
		return true;
	return false;
}

enum dd_data_dir {
	DD_READ		= READ,
	DD_WRITE	= WRITE,
};

enum { DD_DIR_COUNT = 2 };

enum dd_prio {
	DD_HP_PRIO	= 0,
	DD_RT_PRIO	= 1,
	DD_BE_PRIO	= 2,
	DD_PRIO_MAX	= 2,
};

enum { DD_PRIO_COUNT = 3 };

/*
 * I/O statistics per I/O priority. It is fine if these counters overflow.
 * What matters is that these counters are at least as wide as
 * log2(max_outstanding_requests).
 */
struct io_stats_per_prio {
	uint32_t inserted;
	uint32_t merged;
	uint32_t dispatched;
	atomic_t completed;
};

/*
 * Deadline scheduler data per I/O priority (enum dd_prio). Requests are
 * present on both sort_list[] and fifo_list[].
 */
struct dd_per_prio {
	struct smart_io_throttle_ctx *throttle;
	struct list_head dispatch;
	struct rb_root sort_list[DD_DIR_COUNT];
	struct list_head fifo_list[DD_DIR_COUNT];
	struct request *vip_tail[DD_DIR_COUNT];
	/* Position of the most recently dispatched request. */
	sector_t latest_pos[DD_DIR_COUNT];
	struct io_stats_per_prio stats;
};

struct deadline_data {
	/*
	 * run time data
	 */

	struct dd_per_prio per_prio[DD_PRIO_COUNT];
	struct smart_io_throttle_ctx throttle;

	/* Data direction of latest dispatched request. */
	enum dd_data_dir last_dir;
	unsigned int batching;		/* number of sequential requests made */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[DD_DIR_COUNT];
	int fifo_batch;
	int writes_starved;
	int front_merges;
	u32 async_depth;
	int prio_aging_expire;

	spinlock_t lock;
};

/* Maps an I/O priority class to a deadline scheduler priority. */
static const enum dd_prio ioprio_class_to_prio[] = {
	[IOPRIO_CLASS_NONE]	= DD_BE_PRIO,
	[IOPRIO_CLASS_RT]	= DD_RT_PRIO,
	[IOPRIO_CLASS_BE]	= DD_BE_PRIO,
	[IOPRIO_CLASS_IDLE]	= DD_BE_PRIO,
};

static inline struct rb_root *
deadline_rb_root(struct dd_per_prio *per_prio, struct request *rq)
{
	return &per_prio->sort_list[rq_data_dir(rq)];
}

/*
 * Returns the I/O priority class (IOPRIO_CLASS_*) that has been assigned to a
 * request.
 */
static __maybe_unused u8 dd_rq_ioclass(struct request *rq)
{
	return IOPRIO_PRIO_CLASS(req_get_ioprio(rq));
}

/*
 * Return the first request for which blk_rq_pos() >= @pos.
 */
static __maybe_unused inline struct request *deadline_from_pos(struct dd_per_prio *per_prio,
				enum dd_data_dir data_dir, sector_t pos)
{
	struct rb_node *node = per_prio->sort_list[data_dir].rb_node;
	struct request *rq, *res = NULL;

	if (!node)
		return NULL;

	rq = rb_entry_rq(node);
	while (node) {
		rq = rb_entry_rq(node);
		if (blk_rq_pos(rq) >= pos) {
			res = rq;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return res;
}

static void
deadline_add_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(per_prio, rq);

	elv_rb_add(root, rq);
}

static inline void
deadline_del_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	elv_rb_del(deadline_rb_root(per_prio, rq), rq);
}

/*
 * remove rq from rbtree and fifo.
 */
static void deadline_remove_request(struct request_queue *q,
				    struct dd_per_prio *per_prio,
				    struct request *rq)
{
	enum dd_data_dir data_dir = rq_data_dir(rq);

	if (per_prio->vip_tail[data_dir] == rq) {
		per_prio->vip_tail[data_dir] =
		rq->queuelist.prev == &per_prio->fifo_list[data_dir] ?
			NULL : list_entry(rq->queuelist.prev, struct request,
					  queuelist);
	}
	list_del_init(&rq->queuelist);

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node))
		deadline_del_rq_rb(per_prio, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

static void dd_request_merged(struct request_queue *q, struct request *req,
			      enum elv_merge type)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const enum dd_prio prio = (enum dd_prio)
		smart_io_throttle_select_queue(req);
	struct dd_per_prio *per_prio = &dd->per_prio[prio];

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(deadline_rb_root(per_prio, req), req);
		deadline_add_rq_rb(per_prio, req);
	}
	smart_io_throttle_update_request(&dd->throttle, req);
}

/*
 * Callback function that is invoked after @next has been merged into @req.
 */
static void dd_merged_requests(struct request_queue *q, struct request *req,
			       struct request *next)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const enum dd_prio prio = (enum dd_prio)
		smart_io_throttle_select_queue(next);
	struct dd_per_prio *per_prio = &dd->per_prio[prio];

	lockdep_assert_held(&dd->lock);

	smart_io_throttle_update_request(&dd->throttle, req);
	smart_io_throttle_unqueue_request(&dd->throttle, next);
	dd->per_prio[prio].stats.merged++;

	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) {
			if (per_prio->vip_tail[rq_data_dir(next)] == next)
				per_prio->vip_tail[rq_data_dir(next)] = req;
			list_move(&req->queuelist, &next->queuelist);
			req->fifo_time = next->fifo_time;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, &dd->per_prio[prio], next);
}

/*
 * move an entry to dispatch queue
 */
static void
deadline_move_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      struct request *rq)
{
	/*
	 * take it off the sort and fifo list
	 */
	deadline_remove_request(rq->q, per_prio, rq);
}

/* Number of requests queued for a given priority level. */
static u32 dd_queued(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats;

	lockdep_assert_held(&dd->lock);

	return stats->inserted - atomic_read(&stats->completed);
}

static bool dd_has_work_for_prio(struct dd_per_prio *per_prio)
{
	return !list_empty_careful(&per_prio->dispatch) ||
		!list_empty_careful(&per_prio->fifo_list[DD_READ]) ||
		!list_empty_careful(&per_prio->fifo_list[DD_WRITE]);
}

/*
 * deadline_check_fifo returns true if and only if there are expired requests
 * in the FIFO list. Requires !list_empty(&dd->fifo_list[data_dir]).
 */
static inline bool deadline_check_fifo(struct dd_per_prio *per_prio,
				       enum dd_data_dir data_dir)
{
	struct request *rq = rq_entry_fifo(per_prio->fifo_list[data_dir].next);

	return time_is_before_eq_jiffies((unsigned long)rq->fifo_time);
}

/*
 * For the specified data direction, return the next request to
 * dispatch using arrival ordered lists.
 */
static __maybe_unused struct request *
deadline_fifo_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	if (list_empty(&per_prio->fifo_list[data_dir]))
		return NULL;

	return rq_entry_fifo(per_prio->fifo_list[data_dir].next);
}

/*
 * For the specified data direction, return the next request to
 * dispatch using sector position sorted lists.
 */
static __maybe_unused struct request *
deadline_next_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	return deadline_from_pos(per_prio, data_dir,
				 per_prio->latest_pos[data_dir]);
}

/*
 * Returns true if and only if @rq started after @latest_start where
 * @latest_start is in jiffies.
 */
static __maybe_unused bool started_after(struct deadline_data *dd, struct request *rq,
			  unsigned long latest_start)
{
	unsigned long start_time = (unsigned long)rq->fifo_time;

	start_time -= dd->fifo_expire[rq_data_dir(rq)];

	return time_after(start_time, latest_start);
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc and with a start time <= @latest_start.
 */
#if 0 /* Deadline expiry, aging and policy selection are intentionally disabled. */
static struct request *__dd_dispatch_request(struct deadline_data *dd,
					     struct dd_per_prio *per_prio,
					     unsigned long latest_start)
{
	struct request *rq, *next_rq;
	enum dd_data_dir data_dir;
	enum dd_prio prio;
	u8 ioprio_class;

	lockdep_assert_held(&dd->lock);

	if (!list_empty(&per_prio->dispatch)) {
		rq = list_first_entry(&per_prio->dispatch, struct request,
				      queuelist);
		if (started_after(dd, rq, latest_start))
			return NULL;
		list_del_init(&rq->queuelist);
		data_dir = rq_data_dir(rq);
		goto done;
	}

	/*
	 * batches are currently reads XOR writes
	 */
	rq = deadline_next_request(dd, per_prio, dd->last_dir);
	if (rq && dd->batching < dd->fifo_batch) {
		/* we have a next request and are still entitled to batch */
		data_dir = rq_data_dir(rq);
		goto dispatch_request;
	}

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (!list_empty(&per_prio->fifo_list[DD_READ])) {
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_READ]));

		if (deadline_fifo_request(dd, per_prio, DD_WRITE) &&
		    (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

		data_dir = DD_READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (!list_empty(&per_prio->fifo_list[DD_WRITE])) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_WRITE]));

		dd->starved = 0;

		data_dir = DD_WRITE;

		goto dispatch_find_request;
	}

	return NULL;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	next_rq = deadline_next_request(dd, per_prio, data_dir);
	if (deadline_check_fifo(per_prio, data_dir) || !next_rq) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = deadline_fifo_request(dd, per_prio, data_dir);
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = next_rq;
	}

	if (!rq)
		return NULL;

	dd->last_dir = data_dir;
	dd->batching = 0;

dispatch_request:
	if (started_after(dd, rq, latest_start))
		return NULL;

	/*
	 * rq is the selected appropriate request.
	 */
	dd->batching++;
	deadline_move_request(dd, per_prio, rq);
done:
	ioprio_class = dd_rq_ioclass(rq);
	prio = ioprio_class_to_prio[ioprio_class];
	dd->per_prio[prio].latest_pos[data_dir] = blk_rq_pos(rq);
	dd->per_prio[prio].stats.dispatched++;
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

struct dd_candidate {
	struct request *rq;
	struct dd_per_prio *per_prio;
	bool from_dispatch;
};

static struct request *dd_find_class_in_list(struct list_head *list,
					      enum smart_io_rq_class class,
					      struct deadline_data *dd,
					      bool require_seq,
					      unsigned int *budget)
{
	struct request *rq;

	list_for_each_entry(rq, list, queuelist) {
		if (!*budget)
			return NULL;
		(*budget)--;
		if (smart_io_throttle_rq_class(rq) != class)
			continue;
		if (require_seq &&
		    !smart_io_throttle_seq_match(&dd->throttle, rq))
			continue;
		return rq;
	}

	return NULL;
}

static bool dd_find_class_candidate(struct deadline_data *dd,
				    enum smart_io_rq_class class,
				    bool require_seq,
				    struct dd_candidate *candidate)
{
	unsigned int budget = smart_io_throttle_scan_budget(&dd->throttle);
	enum dd_prio prio;
	struct request *read_rq;
	struct request *write_rq;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		read_rq = dd_find_class_in_list(&dd->per_prio[prio].dispatch,
							class, dd, require_seq, &budget);
		if (read_rq) {
			candidate->rq = read_rq;
			candidate->per_prio = &dd->per_prio[prio];
			candidate->from_dispatch = true;
			return true;
		}
	}

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		read_rq = dd_find_class_in_list(
			&dd->per_prio[prio].fifo_list[DD_READ], class, dd,
			require_seq, &budget);
		write_rq = dd_find_class_in_list(
			&dd->per_prio[prio].fifo_list[DD_WRITE], class, dd,
			require_seq, &budget);
		if (!read_rq && !write_rq)
			continue;
		candidate->rq = !read_rq ? write_rq : !write_rq ? read_rq :
			time_before((unsigned long)read_rq->fifo_time,
				    (unsigned long)write_rq->fifo_time) ?
			read_rq : write_rq;
		candidate->per_prio = &dd->per_prio[prio];
		candidate->from_dispatch = false;
		return true;
	}

	return false;
}

static void dd_consider_small_candidate(struct list_head *list,
					 struct dd_per_prio *per_prio,
					 bool from_dispatch,
					 unsigned int *budget,
					 struct dd_candidate *candidate,
					 bool *found, unsigned int *smallest)
{
	struct request *rq;

	list_for_each_entry(rq, list, queuelist) {
		unsigned int bytes;

		if (!*budget)
			return;
		(*budget)--;
		if (smart_io_throttle_rq_class(rq) != SMART_IO_RQ_BACKGROUND)
			continue;
		bytes = blk_rq_bytes(rq);
		if (*found && bytes >= *smallest)
			continue;
		*found = true;
		*smallest = bytes;
		candidate->rq = rq;
		candidate->per_prio = per_prio;
		candidate->from_dispatch = from_dispatch;
	}
}

static bool dd_find_small_background(struct deadline_data *dd,
				     struct dd_candidate *candidate)
{
	unsigned int budget = smart_io_throttle_scan_budget(&dd->throttle);
	unsigned int smallest = 0;
	bool found = false;
	enum dd_prio prio;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		dd_consider_small_candidate(&dd->per_prio[prio].dispatch,
						    &dd->per_prio[prio], true,
						    &budget, candidate, &found, &smallest);
	}
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		dd_consider_small_candidate(&dd->per_prio[prio].fifo_list[DD_READ],
						    &dd->per_prio[prio], false,
						    &budget, candidate, &found, &smallest);
		dd_consider_small_candidate(&dd->per_prio[prio].fifo_list[DD_WRITE],
						    &dd->per_prio[prio], false,
						    &budget, candidate, &found, &smallest);
	}

	return found;
}

static void dd_consider_expired_background(struct list_head *list,
					    struct dd_per_prio *per_prio,
					    bool from_dispatch,
					    struct deadline_data *dd,
					    u64 now_ns,
					    unsigned int *budget,
					    struct dd_candidate *candidate,
					    bool *found,
					    u64 *oldest_ts_ns)
{
	struct request *rq;

	list_for_each_entry(rq, list, queuelist) {
		u64 queued_ts_ns;

		if (!*budget)
			return;
		(*budget)--;
		if (!smart_io_throttle_background_deadline_expired(
				&dd->throttle, rq, now_ns, &queued_ts_ns))
			continue;
		if (*found && queued_ts_ns >= *oldest_ts_ns)
			continue;
		*found = true;
		*oldest_ts_ns = queued_ts_ns;
		candidate->rq = rq;
		candidate->per_prio = per_prio;
		candidate->from_dispatch = from_dispatch;
	}
}

static bool dd_find_expired_background(struct deadline_data *dd, u64 now_ns,
				       struct dd_candidate *candidate)
{
	unsigned int budget = smart_io_throttle_scan_budget(&dd->throttle);
	u64 oldest_ts_ns = 0;
	bool found = false;
	enum dd_prio prio;

	if (!smart_io_throttle_deadline_escape_available(&dd->throttle))
		return false;
	for (prio = 0; prio <= DD_PRIO_MAX; prio++)
		dd_consider_expired_background(&dd->per_prio[prio].dispatch,
					       &dd->per_prio[prio], true, dd,
					       now_ns, &budget, candidate,
					       &found, &oldest_ts_ns);
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		dd_consider_expired_background(
			&dd->per_prio[prio].fifo_list[DD_READ],
			&dd->per_prio[prio], false, dd, now_ns, &budget,
			candidate, &found, &oldest_ts_ns);
		dd_consider_expired_background(
			&dd->per_prio[prio].fifo_list[DD_WRITE],
			&dd->per_prio[prio], false, dd, now_ns, &budget,
			candidate, &found, &oldest_ts_ns);
	}

	return found;
}

static struct request *dd_dispatch_candidate(struct deadline_data *dd,
					       struct dd_candidate *candidate)
{
	struct request *rq = candidate->rq;
	struct dd_per_prio *per_prio = candidate->per_prio;
	enum dd_data_dir data_dir = rq_data_dir(rq);

	if (candidate->from_dispatch)
		list_del_init(&rq->queuelist);
	else
		deadline_move_request(dd, per_prio, rq);
	dd->last_dir = data_dir;
	dd->batching = 1;
	per_prio->latest_pos[data_dir] = blk_rq_pos(rq);
	per_prio->stats.dispatched++;
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

static struct request *dd_dispatch_class(struct deadline_data *dd,
					 enum smart_io_rq_class class,
					 bool require_seq)
{
	struct dd_candidate candidate;

	if (!dd_find_class_candidate(dd, class, require_seq, &candidate))
		return NULL;
	return dd_dispatch_candidate(dd, &candidate);
}

static struct request *dd_dispatch_background_policy(
		struct deadline_data *dd, enum smart_io_dispatch_policy policy,
		enum smart_io_dispatch_selection *selection)
{
	struct dd_candidate candidate;

	if (policy == SMART_IO_DISPATCH_SMALL &&
	    dd_find_small_background(dd, &candidate)) {
		*selection = SMART_IO_SELECTION_SMALL;
		return dd_dispatch_candidate(dd, &candidate);
	}
	if (policy == SMART_IO_DISPATCH_SEQ) {
		if (dd_find_class_candidate(dd, SMART_IO_RQ_BACKGROUND, true,
					    &candidate)) {
			*selection = SMART_IO_SELECTION_SEQ_HIT;
			return dd_dispatch_candidate(dd, &candidate);
		}
		*selection = SMART_IO_SELECTION_SEQ_FALLBACK;
	}
	return NULL;
}

/*
 * Check whether there are any requests with priority other than DD_RT_PRIO
 * that were inserted more than prio_aging_expire jiffies ago.
 */
static struct request *dd_dispatch_prio_aged_requests(struct deadline_data *dd,
						      unsigned long now,
						      enum dd_prio first_prio)
{
	struct request *rq;
	enum dd_prio prio;
	int prio_cnt;

	lockdep_assert_held(&dd->lock);

	prio_cnt = 0;
	for (prio = first_prio; prio <= DD_PRIO_MAX; prio++)
		prio_cnt += !!dd_queued(dd, prio);
	if (prio_cnt < 2)
		return NULL;

	for (prio = first_prio; prio <= DD_PRIO_MAX; prio++) {
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio],
					   now - dd->prio_aging_expire);
		if (rq)
			return rq;
	}

	return NULL;
}

/*
 * Called from blk_mq_run_hw_queue() -> __blk_mq_sched_dispatch_requests().
 *
 * One confusing aspect here is that we get called for a specific
 * hardware queue, but we may return a request that is for a
 * different hardware queue. This is because mq-deadline has shared
 * state for all hardware queues, in terms of sorting, FIFOs, etc.
 */
static struct request *dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	const unsigned long now = jiffies;
	struct dd_candidate deadline_candidate;
	struct request *rq = NULL;
	enum dd_prio prio;
	enum smart_io_dispatch_policy policy;
	enum smart_io_dispatch_selection selection =
		SMART_IO_SELECTION_BACKGROUND_BASELINE;
	bool controlled_work;

	smart_io_throttle_dispatch_model_if_needed(&dd->throttle);
	spin_lock(&dd->lock);
	if (!smart_io_throttle_enabled()) {
		rq = dd_dispatch_prio_aged_requests(dd, now, DD_RT_PRIO);
		if (rq)
			goto account;
		for (prio = DD_RT_PRIO; prio <= DD_PRIO_MAX; prio++) {
			rq = __dd_dispatch_request(dd, &dd->per_prio[prio], now);
			if (rq || dd_queued(dd, prio))
				break;
		}
		goto account;
	}

	rq = dd_dispatch_class(dd, SMART_IO_RQ_FOREGROUND, false);
	if (rq) {
		selection = SMART_IO_SELECTION_FOREGROUND_FIFO;
		goto account;
	}

	if (smart_io_throttle_has_pending(&dd->throttle,
					 SMART_IO_RQ_BACKGROUND) &&
	    smart_io_throttle_background_allowed(&dd->throttle) &&
	    dd_find_expired_background(dd, ktime_get_boottime_ns(),
					 &deadline_candidate)) {
		rq = dd_dispatch_candidate(dd, &deadline_candidate);
		selection = SMART_IO_SELECTION_BACKGROUND_DEADLINE;
		goto account;
	}

	controlled_work = smart_io_throttle_has_pending(
		&dd->throttle, SMART_IO_RQ_BACKGROUND);
	if (!controlled_work ||
	    !smart_io_throttle_background_allowed(&dd->throttle))
		goto unlock;

	policy = smart_io_throttle_effective_policy(&dd->throttle);
	rq = dd_dispatch_background_policy(dd, policy, &selection);
	if (rq)
		goto account;

	rq = dd_dispatch_prio_aged_requests(dd, now, DD_RT_PRIO);
	if (rq)
		goto account;

	/*
	 * Next, dispatch requests in priority order. Ignore lower priority
	 * requests if any higher priority requests are pending.
	 */
	for (prio = DD_RT_PRIO; prio <= DD_PRIO_MAX; prio++) {
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio], now);
		if (rq || dd_queued(dd, prio))
			break;
	}

account:
	if (rq)
		smart_io_throttle_account_dispatch(&dd->throttle, rq, selection);

unlock:
	spin_unlock(&dd->lock);

	return rq;
}
#endif

static struct request *dd_dispatch_fifo(struct deadline_data *dd,
					struct dd_per_prio *per_prio)
{
	struct request *rq;
	struct request *read_rq;
	struct request *write_rq;
	struct request *vip_read_rq;
	struct request *vip_write_rq;
	u64 vip_read_id;
	u64 vip_write_id;

	if (!list_empty(&per_prio->dispatch)) {
		rq = list_first_entry(&per_prio->dispatch, struct request,
					queuelist);
	} else {
		vip_read_rq = list_first_entry_or_null(
			&per_prio->fifo_list[DD_READ], struct request, queuelist);
		if (vip_read_rq &&
		    !is_vip_request(vip_read_rq))
			vip_read_rq = NULL;
		vip_write_rq = list_first_entry_or_null(
			&per_prio->fifo_list[DD_WRITE], struct request, queuelist);
		if (vip_write_rq &&
		    !is_vip_request(vip_write_rq))
			vip_write_rq = NULL;
		if (vip_read_rq || vip_write_rq) {
			if (!vip_read_rq)
				rq = vip_write_rq;
			else if (!vip_write_rq)
				rq = vip_read_rq;
			else {
				vip_read_id = smart_io_throttle_request_id(vip_read_rq);
				vip_write_id = smart_io_throttle_request_id(vip_write_rq);
				rq = vip_read_id && vip_write_id ?
					vip_read_id < vip_write_id ? vip_read_rq :
					vip_write_rq :
					time_before((unsigned long)vip_read_rq->fifo_time,
						    (unsigned long)vip_write_rq->fifo_time) ?
					vip_read_rq : vip_write_rq;
			}
		} else {
			read_rq = list_first_entry_or_null(
				&per_prio->fifo_list[DD_READ], struct request,
				queuelist);
			write_rq = list_first_entry_or_null(
				&per_prio->fifo_list[DD_WRITE], struct request,
				queuelist);
			if (!read_rq)
				rq = write_rq;
			else if (!write_rq)
				rq = read_rq;
			else
				rq = time_before((unsigned long)read_rq->fifo_time,
						 (unsigned long)write_rq->fifo_time) ?
					read_rq : write_rq;
		}
	}
	if (!rq)
		return NULL;

	deadline_move_request(dd, per_prio, rq);
	per_prio->stats.dispatched++;
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

/*
 * Token-aware dispatch for the HP priority queue. The HP queue holds both VIP
 * (UX reads) and non-VIP (foreground regular reads) requests; each has its own
 * in-flight token cap (vip_ok / nonvip_ok). A request whose class is at its
 * cap is skipped so a capped VIP at the head does not head-of-line-block a
 * dispatchable non-VIP behind it. Selection otherwise mirrors
 * dd_dispatch_fifo(). Caller holds dd->lock.
 */
static struct request *dd_dispatch_hp(struct deadline_data *dd,
				      struct dd_per_prio *per_prio,
				      bool vip_ok, bool nonvip_ok)
{
	struct request *rq;
	struct list_head *pos;

	if (!vip_ok && !nonvip_ok)
		return NULL;

	/* Head-inserted request (BLK_MQ_INSERT_AT_HEAD): honor its class. */
	if (!list_empty(&per_prio->dispatch)) {
		rq = list_first_entry(&per_prio->dispatch, struct request,
				      queuelist);
		if ((is_vip_request(rq) && vip_ok) ||
		    (!is_vip_request(rq) && nonvip_ok))
			goto selected;
	}

	/* A VIP at the head of the read/write fifo (highest priority). */
	if (vip_ok) {
		struct request *vr =
			list_first_entry_or_null(&per_prio->fifo_list[DD_READ],
				struct request, queuelist);
		struct request *vw =
			list_first_entry_or_null(&per_prio->fifo_list[DD_WRITE],
				struct request, queuelist);
		u64 vr_id, vw_id;

		if (vr && !is_vip_request(vr))
			vr = NULL;
		if (vw && !is_vip_request(vw))
			vw = NULL;
		if (vr || vw) {
			if (!vr)
				rq = vw;
			else if (!vw)
				rq = vr;
			else {
				vr_id = smart_io_throttle_request_id(vr);
				vw_id = smart_io_throttle_request_id(vw);
				rq = vr_id && vw_id ?
					vr_id < vw_id ? vr : vw :
					time_before((unsigned long)vr->fifo_time,
						    (unsigned long)vw->fifo_time) ?
						vr : vw;
			}
			goto selected;
		}
	}

	/* First non-VIP in the read/write fifo (skip any leading VIPs). */
	if (nonvip_ok) {
		struct request *r = NULL, *w = NULL;

		list_for_each(pos, &per_prio->fifo_list[DD_READ]) {
			rq = list_entry(pos, struct request, queuelist);
			if (!is_vip_request(rq)) {
				r = rq;
				break;
			}
		}
		list_for_each(pos, &per_prio->fifo_list[DD_WRITE]) {
			rq = list_entry(pos, struct request, queuelist);
			if (!is_vip_request(rq)) {
				w = rq;
				break;
			}
		}
		if (r || w) {
			if (!r)
				rq = w;
			else if (!w)
				rq = r;
			else
				rq = time_before((unsigned long)r->fifo_time,
						 (unsigned long)w->fifo_time) ?
					r : w;
			goto selected;
		}
	}

	return NULL;

selected:
	deadline_move_request(dd, per_prio, rq);
	per_prio->stats.dispatched++;
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

static void dd_insert_fifo_request(struct dd_per_prio *per_prio,
					   struct request *rq,
					   enum dd_data_dir data_dir)
{
	struct list_head *head = &per_prio->fifo_list[data_dir];

	if (!is_vip_request(rq)) {
		list_add_tail(&rq->queuelist, head);
		return;
	}

	/* Keep VIP requests ahead of regular HP requests and FIFO within VIP. */
	if (per_prio->vip_tail[data_dir])
		list_add(&rq->queuelist,
			 &per_prio->vip_tail[data_dir]->queuelist);
	else
		list_add(&rq->queuelist, head);
	per_prio->vip_tail[data_dir] = rq;
}

/*
 * Fast-path head dispatch for the no-throttle case. The HP fifo keeps VIP
 * requests at the head (dd_insert_fifo_request), so dispatching the head of
 * the read/write fifo already honors VIP-before-HP ordering without the token
 * checks or the non-VIP list scan done by dd_dispatch_hp(). Head-inserted
 * requests (BLK_MQ_INSERT_AT_HEAD) still take precedence via the dispatch
 * list. Equivalent to dd_dispatch_hp() with both token classes open.
 * Caller holds dd->lock.
 */
static struct request *dd_dispatch_head(struct deadline_data *dd,
					struct dd_per_prio *per_prio)
{
	struct request *rq;

	if (!list_empty(&per_prio->dispatch)) {
		rq = list_first_entry(&per_prio->dispatch, struct request,
					queuelist);
	} else {
		struct request *read_rq = list_first_entry_or_null(
			&per_prio->fifo_list[DD_READ], struct request,
			queuelist);
		struct request *write_rq = list_first_entry_or_null(
			&per_prio->fifo_list[DD_WRITE], struct request,
			queuelist);
		if (!read_rq)
			rq = write_rq;
		else if (!write_rq)
			rq = read_rq;
		else
			rq = time_before((unsigned long)read_rq->fifo_time,
					 (unsigned long)write_rq->fifo_time) ?
				read_rq : write_rq;
	}
	if (!rq)
		return NULL;

	deadline_move_request(dd, per_prio, rq);
	per_prio->stats.dispatched++;
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

static struct request *dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	struct request *rq = NULL;
	bool vip_ok, nonvip_ok;
	enum smart_io_dispatch_selection selection =
		SMART_IO_SELECTION_BACKGROUND_BASELINE;

	smart_io_throttle_dispatch_model_if_needed(&dd->throttle);
	spin_lock(&dd->lock);
	if (!smart_io_throttle_enabled()) {
		/*
		 * No in-flight tokens: dispatch the head of each queue in
		 * HP -> RT -> BE order. VIP requests already sit at the HP
		 * fifo head, so this skips the token check and the list scan
		 * with identical ordering.
		 */
		rq = dd_dispatch_head(dd, &dd->per_prio[DD_HP_PRIO]);
		if (!rq)
			rq = dd_dispatch_head(dd, &dd->per_prio[DD_RT_PRIO]);
		if (!rq)
			rq = dd_dispatch_head(dd, &dd->per_prio[DD_BE_PRIO]);
	} else {
		smart_io_throttle_hp_token_state(&dd->throttle, &vip_ok,
						  &nonvip_ok);
		rq = dd_dispatch_hp(dd, &dd->per_prio[DD_HP_PRIO], vip_ok,
				    nonvip_ok);
		if (!rq && smart_io_throttle_non_hp_allowed(&dd->throttle,
						SMART_IO_QUEUE_RT))
			rq = dd_dispatch_fifo(dd, &dd->per_prio[DD_RT_PRIO]);
		if (!rq && smart_io_throttle_non_hp_allowed(&dd->throttle,
						SMART_IO_QUEUE_BE))
			rq = dd_dispatch_fifo(dd, &dd->per_prio[DD_BE_PRIO]);
	}
	if (rq)
		smart_io_throttle_account_dispatch(&dd->throttle, rq, selection);
	spin_unlock(&dd->lock);

	return rq;
}

/*
 * 'depth' is a number in the range 1..INT_MAX representing a number of
 * requests. Scale it with a factor (1 << bt->sb.shift) / q->nr_requests since
 * 1..(1 << bt->sb.shift) is the range expected by sbitmap_get_shallow().
 * Values larger than q->nr_requests have the same effect as q->nr_requests.
 */
static int dd_to_word_depth(struct blk_mq_hw_ctx *hctx, unsigned int qdepth)
{
	struct sbitmap_queue *bt = &hctx->sched_tags->bitmap_tags;
	const unsigned int nrr = hctx->queue->nr_requests;

	return ((qdepth << bt->sb.shift) + nrr - 1) / nrr;
}

/*
 * Called by __blk_mq_alloc_request(). The shallow_depth value set by this
 * function is used by __blk_mq_get_tag().
 */
static void dd_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	struct deadline_data *dd = data->q->elevator->elevator_data;

	/* Do not throttle synchronous reads. */
	if (op_is_sync(opf) && !op_is_write(opf))
		return;

	/*
	 * Throttle asynchronous requests and writes such that these requests
	 * do not block the allocation of synchronous requests.
	 */
	data->shallow_depth = dd_to_word_depth(data->hctx, dd->async_depth);
}

/* Called by blk_mq_update_nr_requests(). */
static void dd_depth_updated(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	struct blk_mq_tags *tags = hctx->sched_tags;

	dd->async_depth = q->nr_requests;

	sbitmap_queue_min_shallow_depth(&tags->bitmap_tags, 1);
}

/* Called by blk_mq_init_hctx() and blk_mq_init_sched(). */
static int dd_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	dd_depth_updated(hctx);
	return 0;
}

static void dd_exit_sched(struct elevator_queue *e)
{
	struct deadline_data *dd = e->elevator_data;
	enum dd_prio prio;

	smart_io_throttle_queue_exit(&dd->throttle);

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		struct dd_per_prio *per_prio = &dd->per_prio[prio];
		const struct io_stats_per_prio *stats = &per_prio->stats;
		uint32_t queued;

		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_READ]));
		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_WRITE]));

		spin_lock(&dd->lock);
		queued = dd_queued(dd, prio);
		spin_unlock(&dd->lock);

		WARN_ONCE(queued != 0,
			  "statistics for priority %d: i %u m %u d %u c %u\n",
			  prio, stats->inserted, stats->merged,
			  stats->dispatched, atomic_read(&stats->completed));
	}

	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data).
 */
static int dd_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct deadline_data *dd;
	struct elevator_queue *eq;
	enum dd_prio prio;
	int ret = -ENOMEM;

	eq = elevator_alloc(q, e);
	if (!eq)
		return ret;

	dd = kzalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd)
		goto put_eq;

	eq->elevator_data = dd;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		struct dd_per_prio *per_prio = &dd->per_prio[prio];

		per_prio->throttle = &dd->throttle;
		INIT_LIST_HEAD(&per_prio->dispatch);
		INIT_LIST_HEAD(&per_prio->fifo_list[DD_READ]);
		INIT_LIST_HEAD(&per_prio->fifo_list[DD_WRITE]);
		per_prio->sort_list[DD_READ] = RB_ROOT;
		per_prio->sort_list[DD_WRITE] = RB_ROOT;
	}
	dd->fifo_expire[DD_READ] = read_expire;
	dd->fifo_expire[DD_WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->last_dir = DD_WRITE;
	dd->fifo_batch = fifo_batch;
	dd->prio_aging_expire = prio_aging_expire;
	spin_lock_init(&dd->lock);

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

	ret = smart_io_throttle_queue_init(&dd->throttle, q);
	if (ret)
		goto free_dd;
	q->elevator = eq;
	return 0;

free_dd:
	kfree(dd);
put_eq:
	kobject_put(&eq->kobj);
	return ret;
}

/*
 * Try to merge @bio into an existing request. If @bio has been merged into
 * an existing request, store the pointer to that request into *@rq.
 */
static int dd_request_merge(struct request_queue *q, struct request **rq,
			    struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const enum dd_prio prio = (enum dd_prio)
		smart_io_throttle_select_bio_queue(bio);
	struct dd_per_prio *per_prio = &dd->per_prio[prio];
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	if (!dd->front_merges)
		return ELEVATOR_NO_MERGE;

	__rq = elv_rb_find(&per_prio->sort_list[bio_data_dir(bio)], sector);
	if (__rq) {
		BUG_ON(sector != blk_rq_pos(__rq));

		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			if (blk_discard_mergable(__rq))
				return ELEVATOR_DISCARD_MERGE;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to merge a bio into an existing request. This function is called
 * before @bio is associated with a request.
 */
static bool dd_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock(&dd->lock);
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free);
	spin_unlock(&dd->lock);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

/*
 * 找到同 prio/方向里“尾部正好接上新请求起始扇区”的排队请求（back merge 目标）。
 * 请求流会和其它文件交错，不能只看“上一次插入”；rbtree 以起始扇区为键，
 * 这里做前驱查找，再校验尾部是否相接。
 */
static struct request *dd_find_back_merge_target(struct dd_per_prio *per_prio,
						 struct request *rq)
{
	struct rb_node *node = per_prio->sort_list[rq_data_dir(rq)].rb_node;
	struct request *best = NULL;

	while (node) {
		struct request *cur = rb_entry(node, struct request, rb_node);

		if (blk_rq_pos(cur) < blk_rq_pos(rq)) {
			best = cur;
			node = node->rb_right;
		} else {
			node = node->rb_left;
		}
	}

	if (best && blk_rq_pos(best) + blk_rq_sectors(best) == blk_rq_pos(rq))
		return best;
	return NULL;
}

/*
 * add rq to rbtree and fifo
 */
static void dd_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			      blk_insert_t flags, struct list_head *free)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	const enum dd_data_dir data_dir = rq_data_dir(rq);
	struct dd_per_prio *per_prio;
	enum dd_prio prio;

	lockdep_assert_held(&dd->lock);

	/*
	 * RL request-level priority must run before queue selection so the
	 * chosen action is reflected in the queue chosen and the insert trace.
	 * No-op (fixed path) for non-READ and protected requests, or when RL
	 * is disabled.
	 */
	smart_io_rl_classify_request(rq);
	smart_io_throttle_classify_request(&dd->throttle, rq);
	prio = (enum dd_prio)smart_io_throttle_select_queue(rq);
	per_prio = &dd->per_prio[prio];
	if (!rq->elv.priv[0])
		per_prio->stats.inserted++;
	rq->elv.priv[0] = per_prio;

	if (merge_last_insert) {
		struct request *prev = dd_find_back_merge_target(per_prio, rq);

		if (prev)
			q->last_merge = prev;
	}
	if (blk_mq_sched_try_insert_merge(q, rq, free))
		return;

	trace_block_rq_insert(rq);

	if (flags & BLK_MQ_INSERT_AT_HEAD) {
		list_add(&rq->queuelist, &per_prio->dispatch);
		rq->fifo_time = jiffies;
	} else {
		deadline_add_rq_rb(per_prio, rq);

		if (rq_mergeable(rq)) {
			elv_rqhash_add(q, rq);
			if (!q->last_merge)
				q->last_merge = rq;
		}

		/*
		 * set expire time and add to fifo list
		 */
		/* Keep arrival time for FIFO ordering; deadline expiry is disabled. */
		rq->fifo_time = jiffies;
		dd_insert_fifo_request(per_prio, rq, data_dir);
	}
}

/*
 * Called from blk_mq_insert_request() or blk_mq_dispatch_plug_list().
 */
static void dd_insert_requests(struct blk_mq_hw_ctx *hctx,
			       struct list_head *list,
			       blk_insert_t flags)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	LIST_HEAD(free);
	bool inserted_vip = false;

	spin_lock(&dd->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		if (dd_should_inline_dispatch(rq))
			inserted_vip = true;
		dd_insert_request(hctx, rq, flags, &free);
	}
	spin_unlock(&dd->lock);

	blk_mq_free_requests(&free);

	/*
	 * BLOCKING 队列在派发时会经过 blk_mq_run_dispatch_ops()（取 SRCU，
	 * 可能睡眠），因此只在可抢占的进程上下文内联执行，其余情况保持
	 * blk-mq 原有的异步派发。
	 */
	if (inserted_vip && in_task() && preemptible())
		blk_mq_run_hw_queue(hctx, false);
}

/* Callback from inside blk_mq_rq_ctx_init(). */
static void dd_prepare_request(struct request *rq)
{
	rq->elv.priv[0] = NULL;
	smart_io_throttle_prepare_request(rq);
}

/*
 * Callback from inside blk_mq_free_request().
 */
static void dd_finish_request(struct request *rq)
{
	struct dd_per_prio *per_prio = rq->elv.priv[0];
	struct smart_io_throttle_ctx *throttle = per_prio ?
		per_prio->throttle : NULL;

	if (!throttle && rq->q && rq->q->elevator)
		throttle = &((struct deadline_data *)
			rq->q->elevator->elevator_data)->throttle;
	smart_io_throttle_finish_request(throttle, rq);

	/*
	 * The block layer core may call dd_finish_request() without having
	 * called dd_insert_requests(). Skip requests that bypassed I/O
	 * scheduling. See also blk_mq_request_bypass_insert().
	 */
	if (per_prio)
		atomic_inc(&per_prio->stats.completed);
	rq->elv.priv[0] = NULL;
}

static bool dd_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;

	if (!smart_io_throttle_enabled()) {
		enum dd_prio prio;

		for (prio = 0; prio <= DD_PRIO_MAX; prio++)
			if (dd_has_work_for_prio(&dd->per_prio[prio]))
				return true;
		return false;
	}
	if (smart_io_throttle_model_dispatch_needed(&dd->throttle))
		return true;
	if (dd_has_work_for_prio(&dd->per_prio[DD_HP_PRIO]))
		return true;
	if (!dd_has_work_for_prio(&dd->per_prio[DD_RT_PRIO]) &&
	    !dd_has_work_for_prio(&dd->per_prio[DD_BE_PRIO]))
		return false;

	return smart_io_throttle_non_hp_allowed(&dd->throttle, SMART_IO_QUEUE_RT) ||
		smart_io_throttle_non_hp_allowed(&dd->throttle, SMART_IO_QUEUE_BE);
}

/*
 * sysfs parts below
 */
#define SHOW_INT(__FUNC, __VAR)						\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
									\
	return sysfs_emit(page, "%d\n", __VAR);				\
}
#define SHOW_JIFFIES(__FUNC, __VAR) SHOW_INT(__FUNC, jiffies_to_msecs(__VAR))
SHOW_JIFFIES(deadline_read_expire_show, dd->fifo_expire[DD_READ]);
SHOW_JIFFIES(deadline_write_expire_show, dd->fifo_expire[DD_WRITE]);
SHOW_JIFFIES(deadline_prio_aging_expire_show, dd->prio_aging_expire);
SHOW_INT(deadline_writes_starved_show, dd->writes_starved);
SHOW_INT(deadline_front_merges_show, dd->front_merges);
SHOW_INT(deadline_async_depth_show, dd->async_depth);
SHOW_INT(deadline_fifo_batch_show, dd->fifo_batch);
#undef SHOW_INT
#undef SHOW_JIFFIES

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data, __ret;						\
									\
	__ret = kstrtoint(page, 0, &__data);				\
	if (__ret < 0)							\
		return __ret;						\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	*(__PTR) = __CONV(__data);					\
	return count;							\
}
#define STORE_INT(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, (int))
#define STORE_JIFFIES(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, msecs_to_jiffies)
STORE_JIFFIES(deadline_read_expire_store, &dd->fifo_expire[DD_READ], 0, INT_MAX);
STORE_JIFFIES(deadline_write_expire_store, &dd->fifo_expire[DD_WRITE], 0, INT_MAX);
STORE_JIFFIES(deadline_prio_aging_expire_store, &dd->prio_aging_expire, 0, INT_MAX);
STORE_INT(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX);
STORE_INT(deadline_front_merges_store, &dd->front_merges, 0, 1);
STORE_INT(deadline_async_depth_store, &dd->async_depth, 1, INT_MAX);
STORE_INT(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX);
#undef STORE_FUNCTION
#undef STORE_INT
#undef STORE_JIFFIES

#define DD_ATTR(name) \
	__ATTR(name, 0644, deadline_##name##_show, deadline_##name##_store)

static struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(async_depth),
	DD_ATTR(fifo_batch),
	DD_ATTR(prio_aging_expire),
	__ATTR_NULL
};

static struct elevator_type smart_deadline_elevator = {
	.ops = {
		.depth_updated		= dd_depth_updated,
		.limit_depth		= dd_limit_depth,
		.insert_requests	= dd_insert_requests,
		.dispatch_request	= dd_dispatch_request,
		.prepare_request	= dd_prepare_request,
		.finish_request		= dd_finish_request,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.bio_merge		= dd_bio_merge,
		.request_merge		= dd_request_merge,
		.requests_merged	= dd_merged_requests,
		.request_merged		= dd_request_merged,
		.has_work		= dd_has_work,
		.init_sched		= dd_init_sched,
		.exit_sched		= dd_exit_sched,
		.init_hctx		= dd_init_hctx,
	},

	.elevator_attrs = deadline_attrs,
	.elevator_name = "smart-deadline",
	.elevator_alias = "smart-deadline",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("smart-deadline-iosched");

bool smart_deadline_rq_is_foreground(const struct request *rq)
{
	struct elevator_queue *elevator;

	if (!rq || !rq->q)
		return false;

	elevator = READ_ONCE(rq->q->elevator);
	if (!elevator || READ_ONCE(elevator->type) != &smart_deadline_elevator)
		return false;

	/*
	 * Use the queue-class selection (authoritative meta when classified,
	 * otherwise the ioprio hint) instead of the throttle request class,
	 * which requires the throttle meta to be present. This keeps the
	 * UFS high-priority (CP) marking working even when the throttle is
	 * disabled (meta not allocated).
	 */
	return smart_io_throttle_select_queue(rq) == SMART_IO_QUEUE_HP;
}

int smart_deadline_init(void)
{
	return elv_register(&smart_deadline_elevator);
}

void smart_deadline_exit(void)
{
	elv_unregister(&smart_deadline_elevator);
	smart_io_throttle_model_thread_exit();
}
