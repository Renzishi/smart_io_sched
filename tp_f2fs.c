#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include "io_semantics.h"
#include "smart_io_log.h"
#include "tp_f2fs.h"

static void tp_f2fs_gc_begin_cb(void *ignore, struct super_block *sb,
				 int gc_type, bool no_bg_gc,
				 unsigned int nr_free_secs,
				 long long dirty_nodes, long long dirty_dents,
				 long long dirty_imeta, unsigned int free_sec,
				 unsigned int free_seg, int reserved_seg,
				 unsigned int prefree_seg)
{
	(void)ignore;
	(void)sb;
	(void)gc_type;
	(void)no_bg_gc;
	(void)nr_free_secs;
	(void)dirty_nodes;
	(void)dirty_dents;
	(void)dirty_imeta;
	(void)free_sec;
	(void)free_seg;
	(void)reserved_seg;
	(void)prefree_seg;
	f2fs_gc_begin();
}

static void tp_f2fs_gc_end_cb(void *ignore, struct super_block *sb,
			       int ret, int seg_freed, int sec_freed,
			       long long dirty_nodes, long long dirty_dents,
			       long long dirty_imeta, unsigned int free_sec,
			       unsigned int free_seg, int reserved_seg,
			       unsigned int prefree_seg)
{
	(void)ignore;
	(void)sb;
	(void)ret;
	(void)seg_freed;
	(void)sec_freed;
	(void)dirty_nodes;
	(void)dirty_dents;
	(void)dirty_imeta;
	(void)free_sec;
	(void)free_seg;
	(void)reserved_seg;
	(void)prefree_seg;
	f2fs_gc_end();
}

static void tp_f2fs_write_checkpoint_cb(void *ignore, struct super_block *sb,
					int reason, const char *msg)
{
	(void)ignore;
	(void)sb;
	(void)reason;

	if (!msg)
		return;
	if (strstarts(msg, "start"))
		f2fs_cp_begin();
	else if (!strcmp(msg, "finish checkpoint"))
		f2fs_cp_end();
}

struct f2fs_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct f2fs_tracepoint_entry f2fs_tps[] = {
	{ .name = "f2fs_gc_begin", .func = tp_f2fs_gc_begin_cb },
	{ .name = "f2fs_gc_end", .func = tp_f2fs_gc_end_cb },
	{ .name = "f2fs_write_checkpoint", .func = tp_f2fs_write_checkpoint_cb },
};

#define FOR_EACH_F2FS_TP(i) \
	for (i = 0; i < ARRAY_SIZE(f2fs_tps); i++)

static void lookup_f2fs_tracepoints(struct tracepoint *tp, void *ignore)
{
	int i;

	(void)ignore;

	FOR_EACH_F2FS_TP(i) {
		if (!strcmp(f2fs_tps[i].name, tp->name))
			f2fs_tps[i].tp = tp;
	}
}

void unregister_f2fs_tracepoints(void)
{
	int i;

	FOR_EACH_F2FS_TP(i) {
		if (f2fs_tps[i].init) {
			tracepoint_probe_unregister(f2fs_tps[i].tp,
						  f2fs_tps[i].func, NULL);
			f2fs_tps[i].init = false;
		}
	}

	tracepoint_synchronize_unregister();
	smart_io_log_info("f2fs tracepoints unregistered.\n");
}

int register_f2fs_tracepoints(void)
{
	int i;
	int ret;

	for_each_kernel_tracepoint(lookup_f2fs_tracepoints, NULL);

	FOR_EACH_F2FS_TP(i) {
		if (!f2fs_tps[i].tp) {
			smart_io_log_err("tracepoint %s not found\n", f2fs_tps[i].name);
			unregister_f2fs_tracepoints();
			return -EINVAL;
		}

		ret = tracepoint_probe_register(f2fs_tps[i].tp,
					    f2fs_tps[i].func, NULL);
		if (ret) {
			smart_io_log_err("failed to register %s: %d\n",
					 f2fs_tps[i].name, ret);
			unregister_f2fs_tracepoints();
			return ret;
		}
		f2fs_tps[i].init = true;
		smart_io_log_info("tracepoint %s registered.\n", f2fs_tps[i].name);
	}

	return 0;
}