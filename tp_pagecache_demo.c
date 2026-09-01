#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/user_namespace.h>

#include "io_semantics.h"
#include "linux/compiler.h"
#include "smart_io_log.h"
#include "smart_io_types.h"
#include "system_context.h"
#include "trace_instance.h"
#include "tp_pagecache_demo.h"

#define PAGECACHE_DEMO_WINDOW_NS (50ULL * 1000ULL * 1000ULL)
#define PAGECACHE_DEMO_BUDGET_BYTES (256U * 1024U)
#define PAGECACHE_DEMO_BUDGET_WINDOW_COUNT 10U
#define PAGECACHE_DEMO_BUDGET_WINDOW_NS \
	(PAGECACHE_DEMO_WINDOW_NS * PAGECACHE_DEMO_BUDGET_WINDOW_COUNT)
#define PAGECACHE_DEMO_BUDGET_INODES 64U
#define PAGECACHE_DEMO_LOG_BUDGET 64U
#define PAGECACHE_DEMO_OBS_LOG_BUDGET 256U
#define PAGECACHE_DEMO_NODE_INO 1UL
#define PAGECACHE_DEMO_META_INO 2UL

#if 0 /* Static UID/inode/folio ranges are disabled for hot-page testing. */
struct pagecache_demo_hot_range {
	u32 owner_uid;
	u64 inode_hash;
	u64 start_folio;
	u64 end_folio;
	u16 file_ext;
};

/* Cross-launch duplicate-read pages from the 20260816 FP32 dataset. */
static const struct pagecache_demo_hot_range pagecache_demo_hot_ranges[] = {
	/* cloudmusic uid 10274: base.apk */
	{ 10274, 1143914584525004525ULL, 47027, 47029, SMART_IO_FILE_EXT_APK },
	{ 10274, 1143914584525004525ULL, 47384, 47386, SMART_IO_FILE_EXT_APK },
	{ 10274, 1143914584525004525ULL, 47400, 47402, SMART_IO_FILE_EXT_APK },
	{ 10274, 1143914584525004525ULL, 47808, 47810, SMART_IO_FILE_EXT_APK },
	{ 10274, 1143914584525004525ULL, 48412, 48414, SMART_IO_FILE_EXT_APK },
	{ 10274, 1143914584525004525ULL, 51856, 51861, SMART_IO_FILE_EXT_APK },
	/* cloudmusic uid 10274: exploratory base.vdex */
	{ 10274, 1143914584525156945ULL, 6947, 6949, SMART_IO_FILE_EXT_VDEX },
	{ 10274, 1143914584525156945ULL, 7006, 7008, SMART_IO_FILE_EXT_VDEX },
	{ 10274, 1143914584525156945ULL, 7030, 7032, SMART_IO_FILE_EXT_VDEX },
	{ 10274, 1143914584525156945ULL, 7038, 7040, SMART_IO_FILE_EXT_VDEX },
	{ 10274, 1143914584525156945ULL, 7043, 7048, SMART_IO_FILE_EXT_VDEX },
	{ 10274, 1143914584525156945ULL, 7057, 7059, SMART_IO_FILE_EXT_VDEX },
	{ 10274, 1143914584525156945ULL, 7070, 7072, SMART_IO_FILE_EXT_VDEX },
	/* douyin uid 10275: recurring base.vdex */
	{ 10275, 1143914584525115260ULL, 15631, 15633, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 16060, 16062, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 16168, 16170, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 16220, 16222, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 27106, 27108, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 27843, 27845, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 30489, 30491, SMART_IO_FILE_EXT_VDEX },
	{ 10275, 1143914584525115260ULL, 56909, 56911, SMART_IO_FILE_EXT_VDEX },
	/* douyin uid 10275: recurring base.odex */
	{ 10275, 1143914584525115259ULL, 324, 326, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 1275, 1277, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 1683, 1685, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 1923, 1925, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 1933, 1935, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 2433, 2435, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 3344, 3346, SMART_IO_FILE_EXT_ODEX },
	{ 10275, 1143914584525115259ULL, 5844, 5846, SMART_IO_FILE_EXT_ODEX },
	/* taobao uid 10273: exploratory base.vdex/base.odex */
	{ 10273, 1143914584525073872ULL, 1113, 1115, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1164, 1166, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1167, 1169, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1175, 1179, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1185, 1187, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1233, 1235, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1243, 1245, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1265, 1267, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1279, 1282, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1286, 1288, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1290, 1292, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1294, 1296, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1298, 1302, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1305, 1307, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1333, 1335, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1338, 1340, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1354, 1356, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073872ULL, 1367, 1369, SMART_IO_FILE_EXT_VDEX },
	{ 10273, 1143914584525073838ULL, 7418, 7420, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7468, 7470, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7493, 7495, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7545, 7547, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7579, 7581, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7624, 7626, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7795, 7797, SMART_IO_FILE_EXT_ODEX },
	{ 10273, 1143914584525073838ULL, 7823, 7825, SMART_IO_FILE_EXT_ODEX },
};
#endif

static atomic_t pagecache_demo_on = ATOMIC_INIT(0);
static atomic_t pagecache_demo_observe_on = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(pagecache_demo_budget_lock);
static u64 pagecache_demo_log_window_start;
static u32 pagecache_demo_log_count;

struct pagecache_demo_inode_budget {
	u32 uid;
	u64 inode_hash;
	u64 window_start_ns;
	u32 used_bytes;
};

static struct pagecache_demo_inode_budget
	pagecache_demo_inode_budgets[PAGECACHE_DEMO_BUDGET_INODES];

static bool pagecache_demo_identity(struct address_space *mapping,
					    u64 *inode_hash, unsigned long *ino,
					    unsigned int *major, unsigned int *minor,
					    unsigned int *kind)
{
	struct inode *inode;
	dev_t dev;

	if (!mapping || !mapping->host || !mapping->host->i_sb ||
	    !mapping->host->i_sb->s_type ||
	    !mapping->host->i_sb->s_type->name ||
	    strcmp(mapping->host->i_sb->s_type->name, "f2fs"))
		return false;

	inode = mapping->host;
	if (inode->i_ino != PAGECACHE_DEMO_NODE_INO &&
	    inode->i_ino != PAGECACHE_DEMO_META_INO)
		return false;

	dev = inode->i_sb->s_dev;
	if (inode_hash)
		*inode_hash = (u64)inode->i_ino ^ ((u64)dev << 32);
	if (ino)
		*ino = inode->i_ino;
	if (major)
		*major = MAJOR(dev);
	if (minor)
		*minor = MINOR(dev);
	if (kind)
		*kind = inode->i_ino == PAGECACHE_DEMO_NODE_INO ? 1U : 2U;
	return true;
}

static bool pagecache_demo_folio_identity(struct folio *folio, u64 *inode_hash,
						 unsigned long *ino,
						 unsigned int *major,
						 unsigned int *minor,
						 unsigned int *kind)
{
	return folio && pagecache_demo_identity(folio_mapping(folio), inode_hash,
							ino, major, minor, kind);
}

static void tp_pagecache_demo_lookup_cb(void *ignore,
						struct address_space *mapping,
						pgoff_t index, int fgp_flags,
						gfp_t gfp_mask, struct folio *folio)
{
	u64 now;
	unsigned long ino;
	unsigned int major;
	unsigned int minor;
	unsigned int kind;
	u32 fg_uid;

	(void)ignore;
	(void)gfp_mask;
	if (!atomic_read(&pagecache_demo_observe_on) ||
	    !pagecache_demo_identity(mapping, NULL, &ino, &major, &minor,
					      &kind))
		return;

	now = ktime_get_boottime_ns();
	fg_uid = (u32)smart_io_get_fg_uid();
	smart_io_raw_emit("pc_lookup: ts=%llu dev=%u:%u ino=%lu "
			  "type=%u index=%llu present=%u uptodate=%u pages=%u "
			  "fgp_flags=0x%x uid=%u fg_uid=%u is_front=%u pid=%d tgid=%d\n",
			  now, major, minor, ino, kind, (u64)index, folio ? 1U : 0U,
			  folio && folio_test_uptodate(folio) ? 1U : 0U,
			  folio ? (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX) : 0U,
			  fgp_flags, (u32)from_kuid(&init_user_ns, current_uid()), fg_uid,
			  (u32)(from_kuid(&init_user_ns, current_uid()) == fg_uid),
			  current->pid, current->tgid);
}

static void tp_pagecache_demo_delete_cb(void *ignore, struct folio *folio)
{
	u64 now;
	unsigned long ino;
	unsigned int major;
	unsigned int minor;
	unsigned int kind;
	u32 pages;

	(void)ignore;
	if (!atomic_read(&pagecache_demo_observe_on) ||
	    !pagecache_demo_folio_identity(folio, NULL, &ino, &major,
						    &minor, &kind))
		return;

	now = ktime_get_boottime_ns();
	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	smart_io_raw_emit("pc_delete: ts=%llu dev=%u:%u ino=%lu "
			  "type=%u index=%llu pages=%u pid=%d tgid=%d\n",
			  now, major, minor, ino, kind, (u64)folio->index, pages,
			  current->pid, current->tgid);
}

static void tp_pagecache_demo_refault_cb(void *ignore, struct folio *folio)
{
	u64 now;
	unsigned long ino;
	unsigned int major;
	unsigned int minor;
	unsigned int kind;
	u32 pages;

	(void)ignore;
	if (!atomic_read(&pagecache_demo_observe_on) ||
	    !pagecache_demo_folio_identity(folio, NULL, &ino, &major,
						    &minor, &kind))
		return;

	now = ktime_get_boottime_ns();
	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	smart_io_raw_emit("pc_refault: ts=%llu dev=%u:%u ino=%lu "
			  "type=%u index=%llu pages=%u pid=%d tgid=%d\n",
			  now, major, minor, ino, kind, (u64)folio->index, pages,
			  current->pid, current->tgid);
}

void smart_io_pagecache_demo_note_read(u32 uid, u64 inode_hash,
					       u64 folio_index, u16 file_ext,
					       bool is_front)
{
	(void)uid;
	(void)inode_hash;
	(void)folio_index;
	(void)file_ext;
	(void)is_front;
}

static u64 pagecache_demo_inode_hash(struct folio *folio)
{
	struct address_space *mapping;
	struct inode *inode;
	u64 ino;
	u64 dev;

	if (!folio)
		return 0;

	mapping = folio_mapping(folio);
	if (!mapping || !mapping->host || !mapping->host->i_sb)
		return 0;

	inode = mapping->host;
	ino = (u64)inode->i_ino;
	dev = (u64)inode->i_sb->s_dev;
	return ino ^ (dev << 32);
}

static bool pagecache_demo_take_budget(u32 uid, u64 inode_hash, u64 now,
					       u32 bytes, bool *log_action)
{
	unsigned long flags;
	struct pagecache_demo_inode_budget *budget = NULL;
	struct pagecache_demo_inode_budget *oldest = NULL;
	u64 oldest_ns = U64_MAX;
	unsigned int i;
	bool allowed;

	if (log_action)
		*log_action = false;
	if (!uid || !inode_hash || !bytes || bytes > PAGECACHE_DEMO_BUDGET_BYTES)
		return false;

	spin_lock_irqsave(&pagecache_demo_budget_lock, flags);
	if (!pagecache_demo_log_window_start ||
	    now < pagecache_demo_log_window_start ||
	    now - pagecache_demo_log_window_start >=
		PAGECACHE_DEMO_BUDGET_WINDOW_NS) {
		pagecache_demo_log_window_start = now;
		pagecache_demo_log_count = 0;
	}
	for (i = 0; i < ARRAY_SIZE(pagecache_demo_inode_budgets); i++) {
		struct pagecache_demo_inode_budget *entry =
			&pagecache_demo_inode_budgets[i];

		if (entry->uid == uid && entry->inode_hash == inode_hash) {
			budget = entry;
			break;
		}
		if (!entry->uid) {
			if (!budget)
				budget = entry;
			continue;
		}
		if (entry->window_start_ns < oldest_ns) {
			oldest_ns = entry->window_start_ns;
			oldest = entry;
		}
	}
	if (!budget)
		budget = oldest;
	if (!budget) {
		spin_unlock_irqrestore(&pagecache_demo_budget_lock, flags);
		return false;
	}
	if (budget->uid != uid || budget->inode_hash != inode_hash ||
	    !budget->window_start_ns || now < budget->window_start_ns ||
	    now - budget->window_start_ns >= PAGECACHE_DEMO_BUDGET_WINDOW_NS) {
		budget->uid = uid;
		budget->inode_hash = inode_hash;
		budget->window_start_ns = now;
		budget->used_bytes = 0;
	}
	allowed = budget->used_bytes <= PAGECACHE_DEMO_BUDGET_BYTES - bytes;
	if (allowed)
		budget->used_bytes += bytes;
	if (allowed && log_action && pagecache_demo_log_count < PAGECACHE_DEMO_LOG_BUDGET) {
		pagecache_demo_log_count++;
		*log_action = true;
	}
	spin_unlock_irqrestore(&pagecache_demo_budget_lock, flags);
	return allowed;
}

static void tp_pagecache_demo_shrink_cb(void *ignore, struct folio *folio,
					 bool dirty, bool writeback,
					 bool *activate, bool *keep)
{
	u64 inode_hash;
	unsigned long ino;
	unsigned int major;
	unsigned int minor;
	unsigned int kind;
	u32 fg_uid;
	u64 now;
	u16 file_ext;
	u32 bytes;
	bool log_action;

	(void)ignore;
	(void)keep;

	if (!atomic_read(&pagecache_demo_on) || !folio)
		return;
	
	if (dirty || writeback || !activate)
		return;

	now = ktime_get_boottime_ns();
	if (unlikely(atomic_read(&pagecache_demo_observe_on) &&
	    pagecache_demo_folio_identity(folio, NULL, &ino, &major, &minor,
					  &kind)))
		smart_io_raw_emit("pc_shrink: ts=%llu dev=%u:%u ino=%lu "
				  "type=%u index=%llu pages=%u dirty=%u writeback=%u "
				  "pid=%d tgid=%d\n",
				  now, major, minor, ino, kind, (u64)folio->index,
				  (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX),
				  dirty ? 1U : 0U, writeback ? 1U : 0U,
				  current->pid, current->tgid);

	inode_hash = pagecache_demo_inode_hash(folio);
	fg_uid = (u32)smart_io_get_fg_uid();
	file_ext = smart_io_hot_inode_ext(inode_hash);
	if (file_ext == SMART_IO_FILE_EXT_UNKNOWN)
		return;

	bytes = (u32)min_t(unsigned long, folio_size(folio), U32_MAX);
	if (!pagecache_demo_take_budget(fg_uid, inode_hash, now, bytes,
					&log_action))
		return;

	*activate = true;
	if (log_action && atomic_read(&rawdata_trace_enabled))
		smart_io_raw_emit("pc_activate: ts=%llu inode_hash=%llu "
				  "file_ext=%u index=%llu pages=%u budget_pages=%u "
				  "ttl_ms=500\n",
				 now, inode_hash, file_ext, (u64)folio->index,
				 (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX),
				 PAGECACHE_DEMO_BUDGET_BYTES >> PAGE_SHIFT);
}

struct pagecache_demo_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct pagecache_demo_tracepoint_entry pagecache_demo_tps[] = {
	{ .name = "android_vh_filemap_get_folio", .func = tp_pagecache_demo_lookup_cb },
	{ .name = "mm_filemap_delete_from_page_cache", .func = tp_pagecache_demo_delete_cb },
	{ .name = "android_vh_count_workingset_refault", .func = tp_pagecache_demo_refault_cb },
	{ .name = "android_vh_shrink_folio_list", .func = tp_pagecache_demo_shrink_cb },
};

static void pagecache_demo_lookup(struct tracepoint *tp, void *ignore)
{
	unsigned int i;

	(void)ignore;
	for (i = 0; i < ARRAY_SIZE(pagecache_demo_tps); i++) {
		if (!strcmp(pagecache_demo_tps[i].name, tp->name))
			pagecache_demo_tps[i].tp = tp;
	}
}

void smart_io_pagecache_demo_set_enabled(bool enabled)
{
	atomic_set(&pagecache_demo_on, enabled ? 1 : 0);
	if (!enabled) {
		unsigned long flags;

		spin_lock_irqsave(&pagecache_demo_budget_lock, flags);
		pagecache_demo_log_window_start = 0;
		memset(pagecache_demo_inode_budgets,
		       0, sizeof(pagecache_demo_inode_budgets));
		pagecache_demo_log_count = 0;
		spin_unlock_irqrestore(&pagecache_demo_budget_lock, flags);
	}
}

bool smart_io_pagecache_demo_enabled(void)
{
	return atomic_read(&pagecache_demo_on) != 0;
}

void smart_io_pagecache_demo_set_observe(bool enabled)
{
	atomic_set(&pagecache_demo_observe_on, enabled ? 1 : 0);
}

bool smart_io_pagecache_demo_observe_enabled(void)
{
	return atomic_read(&pagecache_demo_observe_on) != 0;
}

int register_pagecache_demo(void)
{
	unsigned int i;
	int ret;

	for_each_kernel_tracepoint(pagecache_demo_lookup, NULL);
	for (i = 0; i < ARRAY_SIZE(pagecache_demo_tps); i++) {
		struct pagecache_demo_tracepoint_entry *entry =
			&pagecache_demo_tps[i];

		if (!entry->tp) {
			smart_io_log_warn("pagecache demo hook unavailable: %s\n",
					  entry->name);
			continue;
		}
		ret = tracepoint_probe_register(entry->tp, entry->func, NULL);
		if (ret) {
			smart_io_log_warn("failed to register pagecache demo hook %s: %d\n",
					  entry->name, ret);
			continue;
		}
		entry->init = true;
	}
	smart_io_log_info("pagecache demo hooks registered\n");
	return 0;
}

void unregister_pagecache_demo(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pagecache_demo_tps); i++) {
		struct pagecache_demo_tracepoint_entry *entry =
			&pagecache_demo_tps[i];

		if (!entry->init)
			continue;
		tracepoint_probe_unregister(entry->tp, entry->func, NULL);
		entry->init = false;
	}
	tracepoint_synchronize_unregister();
	smart_io_pagecache_demo_set_enabled(false);
	smart_io_pagecache_demo_set_observe(false);
	smart_io_log_info("pagecache demo hook unregistered\n");
}
