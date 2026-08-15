#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include "io_semantics.h"
#include "smart_io_log.h"
#include "smart_io_types.h"
#include "system_context.h"
#include "trace_instance.h"
#include "tp_pagecache_demo.h"

#define PAGECACHE_DEMO_WINDOW_NS (100ULL * 1000ULL * 1000ULL)
#define PAGECACHE_DEMO_RECENT_NS (500ULL * 1000ULL * 1000ULL)
#define PAGECACHE_DEMO_BUDGET_PAGES (SZ_64M >> PAGE_SHIFT)
#define PAGECACHE_DEMO_LOG_BUDGET 64U
#define PAGECACHE_DEMO_RECENT_MAX 64U

struct pagecache_demo_hot_range {
	u32 owner_uid;
	u64 inode_hash;
	u64 start_folio;
	u64 end_folio;
	u16 file_ext;
};

/*
 * Baseline duplicate-read intersection with the previous static ranges.
 * Each entry is owned by the observed foreground UID. Runtime matching also
 * requires a foreground read for the same bucket within the last 500 ms.
 */
static const struct pagecache_demo_hot_range pagecache_demo_hot_ranges[] = {
	{ .owner_uid = 10274, .inode_hash = 1143914584525004525ULL, .start_folio = 51856ULL, .end_folio = 51860ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10275, .inode_hash = 1143914584525070367ULL, .start_folio = 0ULL, .end_folio = 4ULL, .file_ext = SMART_IO_FILE_EXT_ODEX },
	{ .owner_uid = 10275, .inode_hash = 1143914584525070367ULL, .start_folio = 8ULL, .end_folio = 8ULL, .file_ext = SMART_IO_FILE_EXT_ODEX },
	{ .owner_uid = 10275, .inode_hash = 1143914584525005061ULL, .start_folio = 63776ULL, .end_folio = 63776ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10275, .inode_hash = 1143914584525005061ULL, .start_folio = 63779ULL, .end_folio = 63780ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10275, .inode_hash = 1143914584525005061ULL, .start_folio = 63785ULL, .end_folio = 63785ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10275, .inode_hash = 1143914584525005061ULL, .start_folio = 63787ULL, .end_folio = 63787ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10275, .inode_hash = 1143914584525005061ULL, .start_folio = 63789ULL, .end_folio = 63789ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10273, .inode_hash = 1143914584525004271ULL, .start_folio = 16128ULL, .end_folio = 16129ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10273, .inode_hash = 1143914584525004271ULL, .start_folio = 16132ULL, .end_folio = 16132ULL, .file_ext = SMART_IO_FILE_EXT_APK },
	{ .owner_uid = 10273, .inode_hash = 1143914584525073838ULL, .start_folio = 11521ULL, .end_folio = 11521ULL, .file_ext = SMART_IO_FILE_EXT_ODEX },
	{ .owner_uid = 10273, .inode_hash = 1143914584525073838ULL, .start_folio = 11524ULL, .end_folio = 11524ULL, .file_ext = SMART_IO_FILE_EXT_ODEX },
	{ .owner_uid = 10273, .inode_hash = 1143914584525073838ULL, .start_folio = 11534ULL, .end_folio = 11534ULL, .file_ext = SMART_IO_FILE_EXT_ODEX },
};

struct pagecache_demo_recent_read {
	u32 uid;
	u64 inode_hash;
	u64 folio_index;
	u64 last_seen_ns;
};

static atomic_t pagecache_demo_on = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(pagecache_demo_budget_lock);
static u64 pagecache_demo_window_start;
static u32 pagecache_demo_used_pages;
static u32 pagecache_demo_log_count;
static struct pagecache_demo_recent_read pagecache_demo_recent[PAGECACHE_DEMO_RECENT_MAX];
static DEFINE_SPINLOCK(pagecache_demo_recent_lock);

void smart_io_pagecache_demo_note_read(u32 uid, u64 inode_hash,
					       u64 folio_index, u16 file_ext,
					       bool is_front)
{
	unsigned long flags;
	u64 now;
	int free_idx = -1;
	int oldest_idx = 0;
	u64 oldest_ns = U64_MAX;
	unsigned int i;

	if (!atomic_read(&pagecache_demo_on) || !is_front || !uid || !inode_hash ||
	    (file_ext != SMART_IO_FILE_EXT_APK &&
	     file_ext != SMART_IO_FILE_EXT_ODEX &&
	     file_ext != SMART_IO_FILE_EXT_VDEX))
		return;

	now = ktime_get_boottime_ns();
	spin_lock_irqsave(&pagecache_demo_recent_lock, flags);
	for (i = 0; i < ARRAY_SIZE(pagecache_demo_recent); i++) {
		struct pagecache_demo_recent_read *entry =
			&pagecache_demo_recent[i];

		if (!entry->uid) {
			if (free_idx < 0)
				free_idx = i;
			continue;
		}
		if (entry->uid == uid && entry->inode_hash == inode_hash &&
		    entry->folio_index == folio_index) {
			entry->last_seen_ns = now;
			spin_unlock_irqrestore(&pagecache_demo_recent_lock, flags);
			return;
		}
		if (entry->last_seen_ns < oldest_ns) {
			oldest_ns = entry->last_seen_ns;
			oldest_idx = i;
		}
	}
	if (free_idx < 0)
		free_idx = oldest_idx;
	pagecache_demo_recent[free_idx].uid = uid;
	pagecache_demo_recent[free_idx].inode_hash = inode_hash;
	pagecache_demo_recent[free_idx].folio_index = folio_index;
	pagecache_demo_recent[free_idx].last_seen_ns = now;
	spin_unlock_irqrestore(&pagecache_demo_recent_lock, flags);
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

static bool pagecache_demo_hot_range_match(u32 uid, u64 inode_hash,
					    struct folio *folio, u16 *file_ext)
{
	u64 folio_start;
	u64 folio_end;
	unsigned int i;

	if (!uid || !inode_hash || !folio || !folio_nr_pages(folio))
		return false;

	folio_start = (u64)folio->index;
	folio_end = folio_start + (u64)folio_nr_pages(folio) - 1;
	for (i = 0; i < ARRAY_SIZE(pagecache_demo_hot_ranges); i++) {
		const struct pagecache_demo_hot_range *range =
			&pagecache_demo_hot_ranges[i];

		if (range->owner_uid != uid || range->inode_hash != inode_hash)
			continue;
		if (folio_start <= range->end_folio &&
		    folio_end >= range->start_folio) {
			if (file_ext)
				*file_ext = range->file_ext;
			return true;
		}
	}

	return false;
}

static bool pagecache_demo_recent_match(u32 uid, u64 inode_hash,
					struct folio *folio, u64 now)
{
	unsigned long flags;
	u64 start;
	u64 end;
	unsigned int i;
	bool matched = false;

	start = (u64)folio->index;
	end = start + (u64)folio_nr_pages(folio) - 1;
	spin_lock_irqsave(&pagecache_demo_recent_lock, flags);
	for (i = 0; i < ARRAY_SIZE(pagecache_demo_recent); i++) {
		struct pagecache_demo_recent_read *entry =
			&pagecache_demo_recent[i];

		if (entry->uid != uid || entry->inode_hash != inode_hash)
			continue;
		if (now < entry->last_seen_ns ||
		    now - entry->last_seen_ns >= PAGECACHE_DEMO_RECENT_NS)
			continue;
		if (start <= entry->folio_index && end >= entry->folio_index) {
			matched = true;
			break;
		}
	}
	spin_unlock_irqrestore(&pagecache_demo_recent_lock, flags);
	return matched;
}

static bool pagecache_demo_take_budget(u64 now, u32 pages, bool *log_action)
{
	unsigned long flags;
	bool allowed = false;

	if (log_action)
		*log_action = false;
	if (!pages || pages > PAGECACHE_DEMO_BUDGET_PAGES)
		return false;

	spin_lock_irqsave(&pagecache_demo_budget_lock, flags);
	if (!pagecache_demo_window_start ||
	    now < pagecache_demo_window_start ||
	    now - pagecache_demo_window_start >= PAGECACHE_DEMO_WINDOW_NS) {
		pagecache_demo_window_start = now;
		pagecache_demo_used_pages = 0;
		pagecache_demo_log_count = 0;
	}
	if (pagecache_demo_used_pages <= PAGECACHE_DEMO_BUDGET_PAGES - pages) {
		pagecache_demo_used_pages += pages;
		if (log_action && pagecache_demo_log_count < PAGECACHE_DEMO_LOG_BUDGET) {
			pagecache_demo_log_count++;
			*log_action = true;
		}
		allowed = true;
	}
	spin_unlock_irqrestore(&pagecache_demo_budget_lock, flags);
	return allowed;
}

static void tp_pagecache_demo_shrink_cb(void *ignore, struct folio *folio,
					 bool dirty, bool writeback,
					 bool *activate, bool *keep)
{
	u64 inode_hash;
	u32 fg_uid;
	u64 now;
	u16 file_ext;
	u32 pages;
	bool log_action;

	(void)ignore;
	(void)keep;

	if (!atomic_read(&pagecache_demo_on) || !folio || dirty || writeback ||
	    !activate)
		return;

	inode_hash = pagecache_demo_inode_hash(folio);
	fg_uid = (u32)smart_io_get_fg_uid();
	file_ext = 0;
	if (!pagecache_demo_hot_range_match(fg_uid, inode_hash, folio, &file_ext))
		return;

	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	now = ktime_get_boottime_ns();
	if (!pagecache_demo_recent_match(fg_uid, inode_hash, folio, now))
		return;
	if (!pagecache_demo_take_budget(now, pages, &log_action))
		return;

	*activate = true;
	if (log_action && atomic_read(&rawdata_trace_enabled))
		smart_io_raw_emit("pagecache_demo_activate: ts=%llu inode_hash=%llu "
				  "file_ext=%u index=%llu pages=%u budget_pages=%u "
				  "ttl_ms=500\n",
				now, inode_hash, file_ext, (u64)folio->index, pages,
				  PAGECACHE_DEMO_BUDGET_PAGES);
}

struct pagecache_demo_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct pagecache_demo_tracepoint_entry pagecache_demo_tp = {
	.name = "android_vh_shrink_folio_list",
	.func = tp_pagecache_demo_shrink_cb,
};

static void pagecache_demo_lookup(struct tracepoint *tp, void *ignore)
{
	(void)ignore;
	if (!strcmp(pagecache_demo_tp.name, tp->name))
		pagecache_demo_tp.tp = tp;
}

void smart_io_pagecache_demo_set_enabled(bool enabled)
{
	atomic_set(&pagecache_demo_on, enabled ? 1 : 0);
	if (!enabled) {
		unsigned long flags;

		spin_lock_irqsave(&pagecache_demo_budget_lock, flags);
		pagecache_demo_window_start = 0;
		pagecache_demo_used_pages = 0;
		pagecache_demo_log_count = 0;
		spin_unlock_irqrestore(&pagecache_demo_budget_lock, flags);
		spin_lock_irqsave(&pagecache_demo_recent_lock, flags);
		memset(pagecache_demo_recent, 0, sizeof(pagecache_demo_recent));
		spin_unlock_irqrestore(&pagecache_demo_recent_lock, flags);
	}
}

bool smart_io_pagecache_demo_enabled(void)
{
	return atomic_read(&pagecache_demo_on) != 0;
}

int register_pagecache_demo(void)
{
	int ret;

	for_each_kernel_tracepoint(pagecache_demo_lookup, NULL);
	if (!pagecache_demo_tp.tp) {
		smart_io_log_warn("pagecache demo hook unavailable\n");
		return 0;
	}

	ret = tracepoint_probe_register(pagecache_demo_tp.tp,
				       pagecache_demo_tp.func, NULL);
	if (ret) {
		smart_io_log_warn("failed to register pagecache demo hook: %d\n", ret);
		return 0;
	}
	pagecache_demo_tp.init = true;
	smart_io_log_info("pagecache demo hook registered\n");
	return 0;
}

void unregister_pagecache_demo(void)
{
	if (!pagecache_demo_tp.init)
		return;

	tracepoint_probe_unregister(pagecache_demo_tp.tp,
				    pagecache_demo_tp.func, NULL);
	pagecache_demo_tp.init = false;
	tracepoint_synchronize_unregister();
	smart_io_pagecache_demo_set_enabled(false);
	smart_io_log_info("pagecache demo hook unregistered\n");
}
