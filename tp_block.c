#include <linux/bio.h>
#include <linux/tracepoint.h>
#include <trace/events/block.h>
#include <linux/string.h>

#include "io_semantics.h"
#include "smart_io_log.h"
#include "smart_io_throttle.h"
#include "smart_io_types.h"
#include "smart_io_prio_policy.h"
#include "tp_filemap.h"
#include "tp_pi.h"
#include "trace_instance.h"

static void tp_rq_insert_cb(void *ignore, struct request *rq)
{
	smart_io_record_insert(rq);
}

static u64 bio_probe_total;
static int bio_probe_proof;

static void tp_bio_queue_cb(void *ignore, struct bio *bio)
{
	if (!bio)
		return;

	/*
	 * Liveness probe, sampled: the trace instance is only enabled around the
	 * launch window, so emitting the first few bios would write into a disabled
	 * instance and be dropped. One in every 20000 keeps the samples spread over
	 * the whole capture so some land inside the window.
	 */
	bio_probe_total++;
	if (bio_probe_proof < 30 && (bio_probe_total % 20000) == 0) {
		bio_probe_proof++;
		smart_io_raw_emit("bio_probe_proof: seq=%d total=%llu op=%u size=%u comm=%s\n",
				  bio_probe_proof, bio_probe_total, bio_op(bio),
				  bio->bi_iter.bi_size, current->comm);
	}

	if (bio_op(bio) == REQ_OP_READ) {
		smart_io_mark_bio_hint(bio);
		return;
	}

	smart_io_mark_bio_write(bio);
}

static void tp_rq_issue_cb(void *ignore, struct request *rq)
{
	smart_io_throttle_record_issue(rq);
	smart_io_record_issue(rq);
}

static void tp_rq_requeue_cb(void *ignore, struct request *rq)
{
	smart_io_throttle_record_requeue(rq);
	smart_io_record_requeue(rq);
}

static void tp_rq_complete_cb(void *ignore, struct request *rq,
			      blk_status_t status, unsigned int nr_bytes)
{
	smart_io_throttle_record_complete(rq, status, nr_bytes);
	smart_io_record_complete(rq, nr_bytes);
}

static void tp_bio_remap_cb(void *ignore, struct bio *bio, dev_t dev,
			    sector_t from)
{
	smart_io_record_bio_remap(bio, dev, from);
}

static void tp_rq_remap_cb(void *ignore, struct request *rq, dev_t dev,
			   sector_t from)
{
	smart_io_record_rq_remap(rq, dev, from);
}

struct tracepoints_table {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct tracepoints_table interests[] = {
	{ .name = "block_bio_queue", .func = tp_bio_queue_cb },
	{ .name = "block_bio_remap", .func = tp_bio_remap_cb },
	{ .name = "block_rq_insert", .func = tp_rq_insert_cb },
	{ .name = "block_rq_issue", .func = tp_rq_issue_cb },
	{ .name = "block_rq_requeue", .func = tp_rq_requeue_cb },
	{ .name = "block_rq_complete", .func = tp_rq_complete_cb },
	{ .name = "block_rq_remap", .func = tp_rq_remap_cb },
};

#define FOR_EACH_INTEREST(i) \
	for (i = 0; i < ARRAY_SIZE(interests); i++)

static void lookup_tracepoints(struct tracepoint *tp, void *ignore)
{
	int i;

	FOR_EACH_INTEREST(i) {
		if (strcmp(interests[i].name, tp->name) == 0)
			interests[i].tp = tp;
	}
}

void unregister_block_tracepoints(void)
{
	int i;

	FOR_EACH_INTEREST(i) {
		if (interests[i].init) {
			tracepoint_probe_unregister(interests[i].tp, interests[i].func, NULL);
			interests[i].init = false;
		}
	}

	tracepoint_synchronize_unregister();
	smart_io_log_info("block tracepoints unregistered.\n");
}

int register_block_tracepoints(void)
{
	int i;
	int ret;

	for_each_kernel_tracepoint(lookup_tracepoints, NULL);

	FOR_EACH_INTEREST(i) {
		if (!interests[i].tp) {
			smart_io_log_err("tracepoint %s not found\n", interests[i].name);
			unregister_block_tracepoints();
			return -EINVAL;
		}

		ret = tracepoint_probe_register(interests[i].tp, interests[i].func, NULL);
		if (ret) {
			smart_io_log_err("failed to register %s: %d\n",
					 interests[i].name, ret);
			unregister_block_tracepoints();
			return ret;
		}
		interests[i].init = true;
		smart_io_log_info("tracepoint %s registered.\n", interests[i].name);
	}

	return 0;
}
