#include <linux/ioprio.h>

#include "block_helper.h"
#include "smart_io_prio_policy.h"
#include "linux/compiler_attributes.h"
#include "smart_io_rl.h"
#include "linux/blk_types.h"
#include "system_context.h"
#include "tp_binder.h"
#include "tp_pi.h"
#include "tp_filemap.h"
#include "tool.h"
#include "smart_io_throttle.h"

/*
 * top-app 进程的所有 bio 默认提升为 IOPRIO_CLASS_RT（mark_bio_rt），且
 * set_private_hint() 会把 IOPRIO_CLASS_NONE 转成 BE。关掉后 class/level 完全
 * 保持原值、只写 hint 位，用于采集"无 class 优先级"的基线：
 * mq-deadline 只认 class，hint 位不影响它的排序。
 */
static bool fg_rt_mark = true;
module_param(fg_rt_mark, bool, 0644);
MODULE_PARM_DESC(fg_rt_mark,
		 "Keep original ioprio class when off (no RT promotion, no NONE->BE)");

bool is_hp_ioprio(u16 ioprio)
{
	return IOPRIO_PRIO_HINT(ioprio) & SMART_IO_HP_IOPRIO_HINT;
}

bool is_vip_ioprio(u16 ioprio)
{
	return (IOPRIO_PRIO_HINT(ioprio) & SMART_IO_VIP_IOPRIO_HINT) || smart_io_rl_vip_action(ioprio);
}

bool has_smart_ioprio_hint(u16 ioprio)
{
	u16 hint = IOPRIO_PRIO_HINT(ioprio);

	return hint & SMART_IO_PRIVATE_HINT_MASK;
}

bool is_vip_request(const struct request *rq)
{
	return rq && req_op((struct request *)rq) == REQ_OP_READ &&
		is_vip_ioprio(req_get_ioprio((struct request *)rq));
}

static bool is_official_hint(u16 hint)
{
	return hint >= IOPRIO_HINT_DEV_DURATION_LIMIT_1 &&
		hint <= IOPRIO_HINT_DEV_DURATION_LIMIT_7;
}

static u16 set_private_hint(u16 ioprio, u16 hint)
{
	u8 ioprio_class = IOPRIO_PRIO_CLASS(ioprio);
	u16 old_hint = IOPRIO_PRIO_HINT(ioprio);

	if (is_official_hint(old_hint))
		return ioprio;

	if (ioprio_class > IOPRIO_CLASS_IDLE)
		return ioprio;
	if (ioprio_class == IOPRIO_CLASS_NONE && fg_rt_mark) {
		ioprio_class = IOPRIO_CLASS_BE;
		ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, IOPRIO_BE_NORM);
	}

	return IOPRIO_PRIO_VALUE_HINT(ioprio_class,
		IOPRIO_PRIO_LEVEL(ioprio), old_hint | hint);
}

static bool __maybe_unused is_special_read(u16 ioprio, blk_opf_t opf,
					   unsigned int op)
{
	return (IOPRIO_PRIO_CLASS(ioprio) == IOPRIO_CLASS_RT ||
		 (opf & (REQ_PRIO | REQ_META | REQ_SYNC)));
}

static bool current_can_mark_hp(u16 ioprio,
						  blk_opf_t opf, unsigned int op)
{
	// return current_uid_is_fg();
	// return current_uid_is_fg() ||
	// 	is_special_read(ioprio, opf, op);
	u16 hint = IOPRIO_PRIO_HINT(ioprio);
	if (hint & (SMART_IO_PGF_IOPRIO_HINT | SMART_IO_PI_IOPRIO_HINT))
		return true;
	return current_is_ux();
}

static bool current_can_mark_vip(u16 ioprio,
						   blk_opf_t opf,
						   unsigned int op,
						   u64 ino)
{
	u16 hint = IOPRIO_PRIO_HINT(ioprio);

	if (hint & (SMART_IO_PGF_IOPRIO_HINT | SMART_IO_PI_IOPRIO_HINT))
		return true;

	if ((opf & REQ_RAHEAD) && ino != 1 && ino != 2)
		return false;

	// return current_uid_is_fg(); // fg vid policy
	return current_is_ux();
}

void mark_bio_rt(struct bio *bio)
{
	if (!fg_rt_mark)
		return;
	if (smart_io_rl_cpu_cgroup_class() != SMART_IO_RL_CGROUP_TOP_APP)
		return;

	bio->bi_ioprio = IOPRIO_PRIO_VALUE_HINT(IOPRIO_CLASS_RT, 0, IOPRIO_PRIO_HINT(bio->bi_ioprio));
}

void mark_bio_hp(struct bio *bio)
{
	if (!current_can_mark_hp(bio->bi_ioprio,
							 bio->bi_opf, bio_op(bio)))
		return;

	bio->bi_ioprio = set_private_hint(bio->bi_ioprio,
		SMART_IO_HP_IOPRIO_HINT);
}

void mark_bio_vip(struct bio *bio)
{
	if (!current_can_mark_vip(bio->bi_ioprio, bio->bi_opf, bio_op(bio), extract_bio_ino(bio)))
		return;

	bio->bi_ioprio = set_private_hint(
		bio->bi_ioprio, SMART_IO_VIP_IOPRIO_HINT);
}

void mark_bio_pgf(struct bio *bio)
{
	if (!smart_io_filemap_fault_active())
		return;

	bio->bi_ioprio = set_private_hint(bio->bi_ioprio,
		SMART_IO_PGF_IOPRIO_HINT);
}

void mark_request_vip_pi(struct request *rq)
{
	u16 ioprio;

	if (!rq)
		return;
	ioprio = req_get_ioprio(rq);
	ioprio = set_private_hint(ioprio,
			SMART_IO_VIP_IOPRIO_HINT | SMART_IO_PI_IOPRIO_HINT);
	rq->ioprio = ioprio;
}

void mark_bio_pi(struct bio *bio)
{
	u16 hint = 0;

	if (smart_io_pi_current_marked() ||
	    smart_io_binder_pi_current_marked())
		hint |= SMART_IO_HP_IOPRIO_HINT |
			SMART_IO_PI_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT;

	if (current_is_fg_main())
		hint |= SMART_IO_MAIN_IOPRIO_HINT;

	if (hint)
		bio->bi_ioprio = set_private_hint(
			bio->bi_ioprio, hint);
}

/*
 * Stamp the scheduler-tag exemption (SMART_IO_OPF_SCHED_EXEMPT) on the bio:
 * VIP/HP-hinted and IOPRIO_CLASS_RT traffic rides the whole tag pool, everything
 * else is background and gets capped by dd_limit_depth(). Must run after every
 * mark_bio_* so it sees the final ioprio hints.
 */
static void mark_bio_sched_exempt(struct bio *bio)
{
	u16 ioprio = bio->bi_ioprio;

	if (is_vip_ioprio(ioprio) || is_hp_ioprio(ioprio) ||
	    IOPRIO_PRIO_CLASS(ioprio) == IOPRIO_CLASS_RT)
		bio->bi_opf |= SMART_IO_OPF_SCHED_EXEMPT;
}

/*
 * Bio-side equivalent of mark_request_vip_pi(): used by the keyio decision,
 * which runs from the bio hook so that the hint is visible before the queue
 * class is latched.
 */
void smart_io_mark_bio_vip_pi(struct bio *bio)
{
	if (!bio)
		return;
	bio->bi_ioprio = set_private_hint(bio->bi_ioprio,
			SMART_IO_VIP_IOPRIO_HINT | SMART_IO_PI_IOPRIO_HINT);
	mark_bio_sched_exempt(bio);
}

void smart_io_mark_bio_hint(struct bio *bio)
{
	mark_bio_pgf(bio);
	mark_bio_hp(bio);
	mark_bio_pi(bio);
	mark_bio_vip(bio);
	/**
	 * 先前的标记依赖RT所以最后对前台请求标记RT优先级兜底
	 */
	mark_bio_rt(bio);
	mark_bio_sched_exempt(bio);
}

/*
 * Foreground synchronous writes. A SYNC write from the foreground app is on the
 * submitting thread's critical path, so with the write floor released
 * (inflight_floor=0) it rides the HP queue instead of the BE backlog; every
 * other write keeps its background route. Writeback (REQ_BACKGROUND), journal
 * and metadata (REQ_META), cache flushes (REQ_PREFLUSH) and anything above 64K
 * are excluded so a write storm from the foreground app cannot take over the HP
 * lane. REQ_FUA stays eligible: it is synchronous by definition.
 *
 * Marking only the HP route bit (not VIP/PGF/PI) keeps such a write in the
 * non-VIP half of the HP queue, where dd_dispatch_hp() orders it by arrival
 * against the foreground reads. With the floor on, nothing is marked at all and
 * the write path is byte-for-byte the baseline.
 */
#define SMART_IO_FG_WRITE_HP_MAX_BYTES	(64 * 1024)

void smart_io_mark_bio_write(struct bio *bio)
{
	if (bio_op(bio) != REQ_OP_WRITE)
		return;
	if (!(bio->bi_opf & REQ_SYNC) ||
	    (bio->bi_opf & (REQ_BACKGROUND | REQ_META | REQ_PREFLUSH)))
		return;
	if (bio->bi_iter.bi_size > SMART_IO_FG_WRITE_HP_MAX_BYTES)
		return;
	if (!current_uid_is_fg())
		return;

	bio->bi_ioprio = set_private_hint(bio->bi_ioprio,
		SMART_IO_HP_IOPRIO_HINT);
	mark_bio_sched_exempt(bio);
}