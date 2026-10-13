#include <linux/ioprio.h>

#include "block_helper.h"
#include "smart_io_prio_policy.h"
#include "smart_io_rl.h"
#include "linux/blk_types.h"
#include "system_context.h"
#include "tp_pi.h"
#include "tp_filemap.h"
#include "tool.h"

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
	if (ioprio_class == IOPRIO_CLASS_NONE) {
		ioprio_class = IOPRIO_CLASS_BE;
		ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, IOPRIO_BE_NORM);
	}

	return IOPRIO_PRIO_VALUE_HINT(ioprio_class,
		IOPRIO_PRIO_LEVEL(ioprio), old_hint | hint);
}

static bool is_special_read(u16 ioprio, blk_opf_t opf,
					   unsigned int op)
{
	return (IOPRIO_PRIO_CLASS(ioprio) == IOPRIO_CLASS_RT ||
		 (opf & (REQ_PRIO | REQ_META | REQ_SYNC)));
}

static bool current_can_mark_hp(u16 ioprio,
						  blk_opf_t opf, unsigned int op)
{
	return current_uid_is_fg() ||
		is_special_read(ioprio, opf, op);
}

static bool current_can_mark_vip(u16 ioprio,
						   blk_opf_t opf,
						   unsigned int op,
						   u64 ino)
{
	u16 hint = IOPRIO_PRIO_HINT(ioprio);

	if ((opf & REQ_RAHEAD) && ino != 1 && ino != 2)
		return false;

	if (hint & (SMART_IO_PGF_IOPRIO_HINT | SMART_IO_PI_IOPRIO_HINT))
		return true;

	return current_is_ux();
}

void mark_bio_rt(struct bio *bio)
{
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

void mark_bio_pi(struct bio *bio)
{
	u16 hint = 0;

	if (smart_io_pi_current_marked())
		hint |= SMART_IO_HP_IOPRIO_HINT |
			SMART_IO_PI_IOPRIO_HINT | SMART_IO_VIP_IOPRIO_HINT;

	if (current_is_fg_main())
		hint |= SMART_IO_MAIN_IOPRIO_HINT;

	if (hint)
		bio->bi_ioprio = set_private_hint(
			bio->bi_ioprio, hint);
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
}