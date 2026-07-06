#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/user_namespace.h>
#include <linux/vm_event_item.h>

#include "io_semantics.h"
#include "smart_io_log.h"
#include "smart_io_types.h"
#include "tp_filemap.h"
#include "trace_instance.h"

// struct filemap_fault_rec {
// 	struct file *file;
// 	pgoff_t index;
// 	u64 start_ts;
// };

// static DEFINE_PER_CPU(struct filemap_fault_rec, filemap_fault_state);

#define FILEMAP_AGG_HASH_BITS 8
#define FILEMAP_AGG_MAX_ENTRIES 1024

struct filemap_agg_entry {
	const char *event;
	u32 uid;
	u64 inode_hash;
	char fs[SMART_IO_FS_TYPE_LEN];
	pgoff_t index;
	pgoff_t last_index;
	u64 pages;
	u64 samples;
	u64 ts_first;
	u64 ts_last;
	bool overflow;
	struct hlist_node node;
};

static DEFINE_HASHTABLE(filemap_agg_hash, FILEMAP_AGG_HASH_BITS);
static DEFINE_SPINLOCK(filemap_agg_lock);
static struct kmem_cache *filemap_agg_cache;
static u32 filemap_agg_entries;

static struct inode *filemap_inode(struct address_space *mapping,
					   struct file *file, struct folio *folio)
{
	if (file && file_inode(file))
		return file_inode(file);
	if (mapping && mapping->host)
		return mapping->host;
	if (folio) {
		struct address_space *fm = folio_mapping(folio);

		if (fm && fm->host)
			return fm->host;
	}
	return NULL;
}

static u32 filemap_agg_key(const char *event, u64 inode_hash, u32 uid)
{
	return hash_ptr(event, FILEMAP_AGG_HASH_BITS) ^
	       hash_64(inode_hash, FILEMAP_AGG_HASH_BITS) ^
	       hash_32(uid, FILEMAP_AGG_HASH_BITS);
}

static bool filemap_ranges_overlap_or_touch(pgoff_t left_first,
					    pgoff_t left_last,
					    pgoff_t right_first,
					    pgoff_t right_last)
{
	if (right_first > left_last)
		return right_first - left_last <= 1;
	if (left_first > right_last)
		return left_first - right_last <= 1;
	return true;
}

static bool filemap_agg_same_key(struct filemap_agg_entry *entry,
				 const char *event, u64 inode_hash, u32 uid)
{
	return entry->event == event && entry->inode_hash == inode_hash &&
	       entry->uid == uid;
}

static void filemap_agg_merge(struct filemap_agg_entry *entry,
			      pgoff_t index, pgoff_t last_index, u32 pages,
			      u64 ts, bool overflow)
{
	if (index < entry->index)
		entry->index = index;
	if (last_index > entry->last_index)
		entry->last_index = last_index;
	entry->pages += pages;
	entry->samples++;
	entry->ts_last = ts;
	entry->overflow |= overflow;
}

static void filemap_fill_identity(struct inode *inode, char *fs, size_t fs_len,
					  u64 *s_dev, u64 *ino)
{
	if (!inode) {
		strscpy(fs, "unknown", fs_len);
		*s_dev = 0;
		*ino = 0;
		return;
	}

	*ino = inode->i_ino;
	if (!inode->i_sb) {
		strscpy(fs, "unknown", fs_len);
		*s_dev = 0;
		return;
	}
	*s_dev = (u64)inode->i_sb->s_dev;
	if (inode->i_sb->s_type && inode->i_sb->s_type->name)
		strscpy(fs, inode->i_sb->s_type->name, fs_len);
	else
		strscpy(fs, "unknown", fs_len);
}

static void filemap_agg_record(const char *event, struct address_space *mapping,
			       struct folio *folio, struct file *file,
			       pgoff_t index, pgoff_t last_index, u32 pages)
{
	struct inode *inode;
	char fs[SMART_IO_FS_TYPE_LEN];
	u64 s_dev;
	u64 ino;
	u64 inode_hash;
	u64 ts;
	u32 uid;
	u32 key;
	struct filemap_agg_entry *entry;
	struct filemap_agg_entry *fallback = NULL;
	struct filemap_agg_entry *new_entry = NULL;
	unsigned long flags;

	if (atomic_read(&rawdata_trace_enabled) == 0)
		return;

	inode = filemap_inode(mapping, file, folio);
	filemap_fill_identity(inode, fs, sizeof(fs), &s_dev, &ino);
	inode_hash = ino ^ (s_dev << 32);
	uid = from_kuid(&init_user_ns, current_uid());
	ts = ktime_get_boottime_ns();
	key = filemap_agg_key(event, inode_hash, uid);

retry:
	fallback = NULL;
	spin_lock_irqsave(&filemap_agg_lock, flags);
	hash_for_each_possible(filemap_agg_hash, entry, node, key) {
		if (!filemap_agg_same_key(entry, event, inode_hash, uid))
			continue;
		if (filemap_ranges_overlap_or_touch(entry->index,
						    entry->last_index,
						    index, last_index)) {
			filemap_agg_merge(entry, index, last_index, pages, ts,
					  false);
			spin_unlock_irqrestore(&filemap_agg_lock, flags);
			if (new_entry)
				kmem_cache_free(filemap_agg_cache, new_entry);
			return;
		}
		if (!fallback)
			fallback = entry;
	}

	if (filemap_agg_entries >= FILEMAP_AGG_MAX_ENTRIES && fallback) {
		filemap_agg_merge(fallback, index, last_index, pages, ts, true);
		spin_unlock_irqrestore(&filemap_agg_lock, flags);
		if (new_entry)
			kmem_cache_free(filemap_agg_cache, new_entry);
		return;
	}

	if (!new_entry) {
		spin_unlock_irqrestore(&filemap_agg_lock, flags);
		if (!filemap_agg_cache)
			return;
		new_entry = kmem_cache_zalloc(filemap_agg_cache, GFP_ATOMIC);
		if (!new_entry)
			return;
		goto retry;
	}

	if (filemap_agg_entries >= FILEMAP_AGG_MAX_ENTRIES) {
		spin_unlock_irqrestore(&filemap_agg_lock, flags);
		kmem_cache_free(filemap_agg_cache, new_entry);
		return;
	}

	new_entry->event = event;
	new_entry->uid = uid;
	new_entry->inode_hash = inode_hash;
	strscpy(new_entry->fs, fs, sizeof(new_entry->fs));
	new_entry->index = index;
	new_entry->last_index = last_index;
	new_entry->pages = pages;
	new_entry->samples = 1;
	new_entry->ts_first = ts;
	new_entry->ts_last = ts;
	new_entry->overflow = false;
	hash_add(filemap_agg_hash, &new_entry->node, key);
	filemap_agg_entries++;
	spin_unlock_irqrestore(&filemap_agg_lock, flags);
}

static void tp_filemap_get_pages_cb(void *ignore, struct address_space *mapping,
				    pgoff_t index, pgoff_t last_index)
{
	u64 nr_pages;

	(void)ignore;
	if (last_index < index)
		return;
	nr_pages = (u64)(last_index - index) + 1;
	if (nr_pages > U32_MAX)
		nr_pages = U32_MAX;

	smart_io_record_filemap_pages((u32)nr_pages);
}

static void tp_filemap_update_page_cb(void *ignore, struct address_space *mapping,
				      struct folio *folio, struct file *file)
{
	u32 pages;
	pgoff_t index;

	(void)ignore;
	if (!folio)
		return;
	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	index = folio->index;
	filemap_agg_record("filemap_update_page", mapping, folio, file, index,
			   index + pages - 1, pages);
	smart_io_record_filemap_miss(pages);
}

static void tp_filemap_add_to_page_cache_cb(void *ignore, struct folio *folio)
{
	u32 pages;

	(void)ignore;
	if (!folio)
		return;
	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	smart_io_record_filemap_cache_add(pages);
}

static void tp_filemap_delete_from_page_cache_cb(void *ignore,
						 struct folio *folio)
{
	u32 pages;
	pgoff_t index;

	(void)ignore;
	if (!folio)
		return;
	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	index = folio->index;
	filemap_agg_record("filemap_delete_page_cache", folio_mapping(folio),
			   folio, NULL, index, index + pages - 1, pages);
	smart_io_record_filemap_cache_delete(pages);
}

static void tp_filemap_refault_cb(void *ignore, struct folio *folio)
{
	u32 pages;
	pgoff_t index;

	(void)ignore;
	if (!folio)
		return;
	pages = (u32)min_t(unsigned long, folio_nr_pages(folio), U32_MAX);
	index = folio->index;
	filemap_agg_record("filemap_refault", folio_mapping(folio), folio, NULL,
			   index, index + pages - 1, pages);
	smart_io_record_filemap_refault(pages);
}

// static void tp_filemap_fault_start_cb(void *ignore, struct vm_fault *vmf)
// {
// 	struct filemap_fault_rec *rec;

// 	(void)ignore;
// 	if (!vmf)
// 		return;
// 	rec = this_cpu_ptr(&filemap_fault_state);
// 	rec->file = vmf->vma ? vmf->vma->vm_file : NULL;
// 	rec->index = vmf->pgoff;
// 	rec->start_ts = ktime_get_boottime_ns();
// 	filemap_emit_raw("filemap_fault_start", NULL, NULL, rec->file,
// 			 rec->index, rec->index, 1, 0);
// }

// static void tp_filemap_fault_end_cb(void *ignore, struct vm_fault *vmf)
// {
// 	struct filemap_fault_rec *rec;
// 	struct file *file = NULL;
// 	pgoff_t index = 0;
// 	u64 now;
// 	u64 wait_us = 0;

// 	(void)ignore;
// 	now = ktime_get_boottime_ns();
// 	rec = this_cpu_ptr(&filemap_fault_state);
// 	if (vmf) {
// 		file = vmf->vma ? vmf->vma->vm_file : NULL;
// 		index = vmf->pgoff;
// 	}
// 	if (!file) {
// 		file = rec->file;
// 		index = rec->index;
// 	}
// 	if (rec->start_ts && now > rec->start_ts)
// 		wait_us = div64_u64(now - rec->start_ts, 1000);
// 	filemap_emit_raw("filemap_fault_end", NULL, NULL, file, index, index, 1,
// 			 wait_us);
// 	smart_io_record_filemap_fault(wait_us);
// 	rec->file = NULL;
// 	rec->index = 0;
// 	rec->start_ts = 0;
// }

void smart_io_filemap_agg_flush(void)
{
	HLIST_HEAD(flush_list);
	struct filemap_agg_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	int bucket;

	if (READ_ONCE(filemap_agg_entries) == 0)
		return;

	if (atomic_read(&rawdata_trace_enabled) == 0) {
		spin_lock_irqsave(&filemap_agg_lock, flags);
		hash_for_each_safe(filemap_agg_hash, bucket, tmp, entry, node) {
			hash_del(&entry->node);
			kmem_cache_free(filemap_agg_cache, entry);
		}
		filemap_agg_entries = 0;
		spin_unlock_irqrestore(&filemap_agg_lock, flags);
		return;
	}

	spin_lock_irqsave(&filemap_agg_lock, flags);
	hash_for_each_safe(filemap_agg_hash, bucket, tmp, entry, node) {
		hash_del(&entry->node);
		hlist_add_head(&entry->node, &flush_list);
	}
	filemap_agg_entries = 0;
	spin_unlock_irqrestore(&filemap_agg_lock, flags);

	hlist_for_each_entry_safe(entry, tmp, &flush_list, node) {
		smart_io_raw_emit("%s: ts_first=%llu ts_last=%llu "
					"uid=%u inode_hash=%llu index=%llu last_index=%llu "
					"pages=%llu samples=%llu fs=%s overflow=%u\n",
					entry->event, entry->ts_first,
					entry->ts_last, entry->uid,
					entry->inode_hash, (u64)entry->index,
					(u64)entry->last_index, entry->pages,
					entry->samples, entry->fs,
					entry->overflow ? 1 : 0);
	}

	hlist_for_each_entry_safe(entry, tmp, &flush_list, node) {
		hlist_del(&entry->node);
		kmem_cache_free(filemap_agg_cache, entry);
	}
}

struct filemap_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
	bool required;
};

static struct filemap_tracepoint_entry filemap_tps[] = {
	{ .name = "mm_filemap_get_pages", .func = tp_filemap_get_pages_cb, .required = true },
	{ .name = "android_vh_filemap_update_page", .func = tp_filemap_update_page_cb, .required = true },
	{ .name = "mm_filemap_add_to_page_cache", .func = tp_filemap_add_to_page_cache_cb },
	{ .name = "mm_filemap_delete_from_page_cache", .func = tp_filemap_delete_from_page_cache_cb },
	{ .name = "android_vh_count_workingset_refault", .func = tp_filemap_refault_cb },
};

#define FOR_EACH_FILEMAP_TP(i) \
	for (i = 0; i < ARRAY_SIZE(filemap_tps); i++)

static void lookup_filemap_tracepoints(struct tracepoint *tp, void *ignore)
{
	int i;

	(void)ignore;
	FOR_EACH_FILEMAP_TP(i) {
		if (!strcmp(filemap_tps[i].name, tp->name))
			filemap_tps[i].tp = tp;
	}
}

void unregister_filemap_tracepoints(void)
{
	int i;

	FOR_EACH_FILEMAP_TP(i) {
		if (filemap_tps[i].init) {
			tracepoint_probe_unregister(filemap_tps[i].tp,
						filemap_tps[i].func, NULL);
			filemap_tps[i].init = false;
		}
	}
	tracepoint_synchronize_unregister();
	smart_io_filemap_agg_flush();
	kmem_cache_destroy(filemap_agg_cache);
	filemap_agg_cache = NULL;
}

int register_filemap_tracepoints(void)
{
	int i;
	int ret;
	int registered = 0;

	if (!filemap_agg_cache) {
		filemap_agg_cache = KMEM_CACHE(filemap_agg_entry,
					       SLAB_HWCACHE_ALIGN);
		if (!filemap_agg_cache)
			return -ENOMEM;
	}

	for_each_kernel_tracepoint(lookup_filemap_tracepoints, NULL);
	FOR_EACH_FILEMAP_TP(i) {
		if (!filemap_tps[i].tp) {
			if (filemap_tps[i].required) {
				smart_io_log_warn("tracepoint %s not found, filemap metric disabled.\n",
						  filemap_tps[i].name);
				unregister_filemap_tracepoints();
				return -ENOENT;
			}
			smart_io_log_info("optional tracepoint %s not found.\n",
					  filemap_tps[i].name);
			continue;
		}
		ret = tracepoint_probe_register(filemap_tps[i].tp,
					    filemap_tps[i].func, NULL);
		if (ret) {
			if (filemap_tps[i].required) {
				smart_io_log_warn("failed to register %s: %d, filemap metric disabled.\n",
						  filemap_tps[i].name, ret);
				unregister_filemap_tracepoints();
				return ret;
			}
			smart_io_log_warn("failed to register optional %s: %d.\n",
					  filemap_tps[i].name, ret);
			continue;
		}
		filemap_tps[i].init = true;
		registered++;
	}
	smart_io_log_info("filemap tracepoints registered: %d.\n", registered);
	return 0;
}
