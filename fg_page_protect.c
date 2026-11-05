#include <linux/bio.h>
#include <linux/blk-mq.h>
#include <linux/errno.h>
#include <linux/ioprio.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include "smart_io_log.h"
#include "smart_io_prio_policy.h"
#include "smart_io_types.h"
#include "trace_instance.h"

static int fg_page_protect_on;
module_param(fg_page_protect_on, int, 0644);
MODULE_PARM_DESC(fg_page_protect_on,
		 "Enable VIP page protect (0=off, 1=on)");

/*
 * Which insert-marked reads earn the folio mark. Mode 0 reproduces the original
 * VIP-only behaviour. Modes 1/2 exist because REQ_RAHEAD reads are excluded from
 * VIP by current_can_mark_vip(), yet those are the reads that dominate
 * same-launch re-reads.
 */
static int fgpp_mark_mode;
module_param(fgpp_mark_mode, int, 0644);
MODULE_PARM_DESC(fgpp_mark_mode,
		 "Mark criterion: 0=VIP, 1=HP or VIP, 2=RT class or HP or VIP");

/*
 * With the mark left in place, one folio is re-protected every time shrink
 * reaches it. Setting this makes protection a single chance per folio.
 */
static int fgpp_oneshot;
module_param(fgpp_oneshot, int, 0644);
MODULE_PARM_DESC(fgpp_oneshot,
		 "Drop the mark after the first protection (0=keep, 1=one chance)");

static bool fgpp_mark_this(const struct request *rq)
{
	u16 ioprio;

	if (req_op((struct request *)rq) != REQ_OP_READ)
		return false;

	if (fgpp_mark_mode <= 0)
		return is_vip_request(rq);

	ioprio = req_get_ioprio((struct request *)rq);
	if (is_vip_ioprio(ioprio) || is_hp_ioprio(ioprio))
		return true;

	return fgpp_mark_mode >= 2 &&
		IOPRIO_PRIO_CLASS(ioprio) == IOPRIO_CLASS_RT;
}

static void fgpp_rq_insert_cb(void *data, struct request *rq)
{
	struct bio *bio;
	struct bio_vec bvl;
	struct bvec_iter iter;
	struct folio *folio;

	(void)data;
	if (!fg_page_protect_on || !rq)
		return;
	if (!fgpp_mark_this(rq))
		return;

	__rq_for_each_bio(bio, rq) {
		bio_for_each_segment(bvl, bio, iter) {
			folio = page_folio(bvl.bv_page);
			if (folio_test_anon(folio))
				continue;
			if (folio_test_swapcache(folio))
				continue;
			set_bit(PG_oem_reserved_1, &folio->flags);
		}
	}
}

static void fgpp_shrink_cb(void *data, struct folio *folio,
			   bool dirty, bool writeback,
			   bool *activate, bool *keep)
{
	(void)data;
	(void)keep;

	if (!fg_page_protect_on || !folio)
		return;
	if (dirty || writeback || !activate)
		return;
	if (!test_bit(PG_oem_reserved_1, &folio->flags))
		return;

	*activate = true;
	if (fgpp_oneshot)
		clear_bit(PG_oem_reserved_1, &folio->flags);
	if (atomic_read(&rawdata_trace_enabled))
		smart_io_raw_emit("fgpp_protect: ts=%llu index=%llu pages=%u\n",
				  ktime_get_boottime_ns(),
				  (u64)folio->index,
				  (u32)min_t(unsigned long, folio_nr_pages(folio),
					     U32_MAX));
}

static void fgpp_delete_cb(void *data, struct folio *folio)
{
	(void)data;
	if (!fg_page_protect_on || !folio)
		return;
	clear_bit(PG_oem_reserved_1, &folio->flags);
}

struct fgpp_tp_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct fgpp_tp_entry fgpp_tps[] = {
	{ .name = "block_rq_insert", .func = fgpp_rq_insert_cb },
	{ .name = "android_vh_shrink_folio_list", .func = fgpp_shrink_cb },
	{ .name = "mm_filemap_delete_from_page_cache", .func = fgpp_delete_cb },
};

static void fgpp_lookup(struct tracepoint *tp, void *ignore)
{
	unsigned int i;

	(void)ignore;
	for (i = 0; i < ARRAY_SIZE(fgpp_tps); i++) {
		if (!strcmp(fgpp_tps[i].name, tp->name))
			fgpp_tps[i].tp = tp;
	}
}

int register_fg_page_protect(void)
{
	unsigned int i;
	int ret;

	for_each_kernel_tracepoint(fgpp_lookup, NULL);
	for (i = 0; i < ARRAY_SIZE(fgpp_tps); i++) {
		struct fgpp_tp_entry *entry = &fgpp_tps[i];

		if (!entry->tp) {
			smart_io_log_warn("fg_page_protect: tracepoint %s not found\n",
					  entry->name);
			continue;
		}
		ret = tracepoint_probe_register(entry->tp, entry->func, NULL);
		if (ret) {
			smart_io_log_warn("fg_page_protect: failed to register %s: %d\n",
					  entry->name, ret);
			continue;
		}
		entry->init = true;
	}
	smart_io_log_info("fg_page_protect hooks registered\n");
	return 0;
}

void unregister_fg_page_protect(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fgpp_tps); i++) {
		if (fgpp_tps[i].init) {
			tracepoint_probe_unregister(fgpp_tps[i].tp,
						    fgpp_tps[i].func, NULL);
			fgpp_tps[i].init = false;
		}
	}
	tracepoint_synchronize_unregister();
	smart_io_log_info("fg_page_protect hooks unregistered\n");
}
