#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/hashtable.h>
#include <linux/hash.h>
#include <linux/ioprio.h>
#include <linux/kernel.h>
#include <linux/kdev_t.h>
#include <linux/limits.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/user_namespace.h>
#include <linux/swap.h>
#include <linux/vmstat.h>

#include <block/blk-mq.h>
#include <kernel/sched/sched.h>

#include "block_helper.h"
#include "io_semantics.h"
#include "smart_io_log.h"
#include "smart_io_policy.h"
#include "system_context.h"
#include "trace_instance.h"
#include "tool.h"

#define NR_RQ_SHARDS 8
#define RQ_HASH_BITS 12
#define REMAP_HASH_BITS 10
#define HIST_BUCKETS 21
#define HOT_INODE_MAX 64
#define DEFAULT_QUEUE_DEPTH 128
#define SMART_IO_TICK_MS 100U
#define DEFAULT_BLK_MAX_BW_KBPS (800U * 1024U)

struct rq_record {
	struct hlist_node node;
	struct request *rq;
	struct rcu_head rcu;
	struct smart_io_event event;
	bool issued;
	u32 data_bytes;
};

struct rq_shard {
	DECLARE_HASHTABLE(hash, RQ_HASH_BITS);
	spinlock_t lock;
};

struct bio_remap_entry {
	struct hlist_node node;
	struct bio *bio;
	dev_t old_dev;
};

struct rq_remap_entry {
	struct hlist_node node;
	struct request *rq;
	dev_t old_dev;
};


struct uid_io_entry {
	u32 uid;
	u32 read_kb;
	u32 write_kb;
	struct hlist_node node;
};

struct uid_io_bucket {
	spinlock_t lock;
	struct hlist_head hash[1 << SMART_IO_UID_HASH_BITS];
};

struct inode_count_entry {
	u64 ino;
	u64 count;
	struct hlist_node node;
};

struct inode_bucket {
	spinlock_t lock;
	struct hlist_head hash[1 << SMART_IO_INO_HASH_BITS];
};

struct lat_hist {
	u64 count[HIST_BUCKETS];
	u64 sum_us;
	u64 total;
};

struct percpu_bucket {
	spinlock_t lock;
	struct lat_hist read_hist;
	struct lat_hist write_hist;
	u64 io_count;
	u64 byte_kb;
	u64 read_kb;
	u64 write_kb;
	u64 filemap_read_pages;
	u64 filemap_miss_pages;
	u64 filemap_cache_add_pages;
	u64 filemap_cache_delete_pages;
	u64 filemap_refault_pages;
	u64 filemap_fault_count;
	u64 filemap_fault_wait_us;
};

struct completed_bucket {
	u32 p50_rlat;
	u32 p99_rlat;
	u32 avg_rlat;
	u32 p50_wlat;
	u32 p99_wlat;
	u32 avg_wlat;
	u32 ratio_r;
	u32 ratio_w;
	u64 reclaim_diff;
	u32 burst_total_kbps;
	u32 blk_bw_util;
	u32 page_cache_hit_pct;
	u64 file_access_pages;
	u64 file_miss_pages;
	u64 file_cache_add_pages;
	u64 file_cache_delete_pages;
	u64 file_refault_pages;
	u64 file_fault_count;
	u64 file_fault_wait_us;
};

struct remap_info {
	dev_t first_dev;
	bool found;
	bool mixed;
};

DEFINE_PER_CPU(struct smart_io_counters, smart_io_cnts);

static struct rq_shard rq_shards[NR_RQ_SHARDS];
static DEFINE_HASHTABLE(bio_remap_hash, REMAP_HASH_BITS);
static DEFINE_SPINLOCK(bio_remap_lock);
static DEFINE_HASHTABLE(rq_remap_hash, REMAP_HASH_BITS);
static DEFINE_SPINLOCK(rq_remap_lock);
static DEFINE_PER_CPU(struct uid_io_bucket, uid_io_curr);
static struct hlist_head uid_io_prev[1 << SMART_IO_UID_HASH_BITS];
static DEFINE_RWLOCK(uid_io_prev_lock);
static DEFINE_PER_CPU(struct inode_bucket, inode_curr);
static DEFINE_PER_CPU(struct percpu_bucket, cur_bucket);

static struct kmem_cache *rq_record_cache;
static struct kmem_cache *bio_remap_cache;
static struct kmem_cache *rq_remap_cache;
static struct kmem_cache *uid_io_cache;
static struct kmem_cache *inode_cache;

static struct completed_bucket last_bucket;
static DEFINE_SPINLOCK(last_bucket_lock);
static struct smart_io_event latest_event;
static DEFINE_SPINLOCK(latest_event_lock);

static u64 hot_inodes[HOT_INODE_MAX];
static int hot_inode_count;
static DEFINE_SPINLOCK(hot_inode_lock);

static atomic_t in_flight = ATOMIC_INIT(0);
static u32 window_sz_ms = SMART_IO_TICK_MS;
static u32 queue_depth = DEFAULT_QUEUE_DEPTH;
/* request_queue has size/depth limits, not a portable peak throughput. */
static u32 blk_max_bw_kbps = DEFAULT_BLK_MAX_BW_KBPS;
static u64 busy_ns;
static u64 ts_busy_start;
static u64 util_window_start;
static DEFINE_SPINLOCK(util_lock);

static atomic_t f2fs_gc_active = ATOMIC_INIT(0);
static atomic_t f2fs_cp_active = ATOMIC_INIT(0);
static u32 cached_loadavg_1m_x100;
static u32 cached_psi_io;
static u32 cached_render_avg_lat;
static u32 cached_jank_cnt;
static u64 prev_reclaim_pages;

static bool raw_mode_enabled(void)
{
	return atomic_read(&rawdata_trace_enabled) != 0;
}

u32 get_window_sz_ms(void)
{
	return READ_ONCE(window_sz_ms);
}

void set_window_sz_ms(u32 sz_ms)
{
	WRITE_ONCE(window_sz_ms, sz_ms);
}

u32 get_nr_bio(struct request *rq)
{
	struct bio *bio;
	u32 count = 0;

	if (!rq)
		return 0;

	__rq_for_each_bio(bio, rq) {
		count++;
	}
	return count;
}

static u32 get_nr_segment(struct request *rq)
{
	if (!rq)
		return 0;

	return blk_rq_nr_phys_segments(rq);
}

static int get_rq_tag(struct request *rq)
{
	if (!rq)
		return BLK_MQ_NO_TAG;

	if (rq->tag != BLK_MQ_NO_TAG)
		return rq->tag;

	return rq->internal_tag;
}

static u32 get_rq_tag_depth_max(struct request *rq)
{
	if (!rq || !rq->q)
		return 0;

	return blk_queue_depth(rq->q);
}

static int get_rq_tag_depth_cur(struct request *rq)
{
	struct blk_mq_hw_ctx *hctx;

	if (!rq)
		return -1;

	hctx = READ_ONCE(rq->mq_hctx);
	if (!hctx)
		return -1;

	return __blk_mq_active_requests(hctx);
}

static struct rq_record *rq_find_locked(struct rq_shard *shard, struct request *rq)
{
	struct rq_record *rec;

	hash_for_each_possible(shard->hash, rec, node, (unsigned long)rq) {
		if (rec->rq == rq)
			return rec;
	}
	return NULL;
}

static struct bio_remap_entry *bio_remap_find_locked(struct bio *bio)
{
	struct bio_remap_entry *entry;

	hash_for_each_possible(bio_remap_hash, entry, node, (unsigned long)bio) {
		if (entry->bio == bio)
			return entry;
	}
	return NULL;
}

static struct rq_remap_entry *rq_remap_find_locked(struct request *rq)
{
	struct rq_remap_entry *entry;

	hash_for_each_possible(rq_remap_hash, entry, node, (unsigned long)rq) {
		if (entry->rq == rq)
			return entry;
	}
	return NULL;
}

static void resolve_remap_name(dev_t dev, char *buf, size_t buf_len)
{
	char dev_buf[SMART_IO_DEV_NAME_LEN];

	if (!dev) {
		strscpy(buf, "none", buf_len);
		return;
	}

	format_dev_t(dev_buf, dev);
	strscpy(buf, dev_buf, buf_len);
}

static void remap_info_consider(struct remap_info *info, dev_t old_dev)
{
	if (!info || !old_dev)
		return;

	if (!info->found) {
		info->first_dev = old_dev;
		info->found = true;
		return;
	}

	if (info->first_dev != old_dev)
		info->mixed = true;
}

static void consume_rq_remap(struct request *rq, struct remap_info *info)
{
	struct rq_remap_entry *entry;
	unsigned long flags;

	if (!rq || !rq_remap_cache)
		return;

	spin_lock_irqsave(&rq_remap_lock, flags);
	entry = rq_remap_find_locked(rq);
	if (entry) {
		remap_info_consider(info, entry->old_dev);
		hash_del(&entry->node);
		kmem_cache_free(rq_remap_cache, entry);
	}
	spin_unlock_irqrestore(&rq_remap_lock, flags);
}

static void consume_bio_remaps(struct request *rq, struct remap_info *info)
{
	struct bio *bio;
	unsigned long flags;

	if (!rq || !bio_remap_cache)
		return;

	__rq_for_each_bio(bio, rq) {
		struct bio_remap_entry *entry;

		spin_lock_irqsave(&bio_remap_lock, flags);
		entry = bio_remap_find_locked(bio);
		if (entry) {
			remap_info_consider(info, entry->old_dev);
			hash_del(&entry->node);
			kmem_cache_free(bio_remap_cache, entry);
		}
		spin_unlock_irqrestore(&bio_remap_lock, flags);
	}
}

static void consume_request_remap(struct request *rq, struct remap_info *info)
{
	if (!info)
		return;

	memset(info, 0, sizeof(*info));
	consume_rq_remap(rq, info);
	consume_bio_remaps(rq, info);
}

static void apply_remap_info(struct smart_io_event *evt,
			     const struct remap_info *info)
{
	if (!evt || !info || !info->found)
		return;

	resolve_remap_name(info->first_dev, evt->remap_from,
			   sizeof(evt->remap_from));
	evt->remap_mixed = info->mixed ? 1 : 0;
}

static u32 calc_blk_bw_util(u32 bandwidth_kbps)
{
	u32 max_bw_kbps = READ_ONCE(blk_max_bw_kbps);
	if (max_bw_kbps <= bandwidth_kbps) {
		WRITE_ONCE(blk_max_bw_kbps, bandwidth_kbps);
		return 9999;
	}
	return calc_util_pct(bandwidth_kbps, max_bw_kbps);
}

static u32 uid_io_prev_bandwidth_kbps(u32 uid)
{
	struct uid_io_entry *entry;
	unsigned int idx = uid_hash_idx(uid);
	unsigned long flags;
	u64 kb = 0;

	if (raw_mode_enabled())
		return 0;

	read_lock_irqsave(&uid_io_prev_lock, flags);
	hlist_for_each_entry(entry, &uid_io_prev[idx], node) {
		if (entry->uid == uid) {
			kb = (u64)entry->read_kb + entry->write_kb;
			break;
		}
	}
	read_unlock_irqrestore(&uid_io_prev_lock, flags);

	return kb_window_to_kbps(kb, window_sz_ms);
}

static void uid_io_curr_add(u32 uid, u32 kb, u8 op)
{
	struct uid_io_bucket *bucket;
	struct uid_io_entry *entry;
	struct uid_io_entry *new_entry = NULL;
	unsigned int idx = uid_hash_idx(uid);
	unsigned long flags;

retry:
	bucket = this_cpu_ptr(&uid_io_curr);
	spin_lock_irqsave(&bucket->lock, flags);
	hlist_for_each_entry(entry, &bucket->hash[idx], node) {
		if (entry->uid == uid) {
			if (io_op_is_write(op))
				entry->write_kb += kb;
			else
				entry->read_kb += kb;
			spin_unlock_irqrestore(&bucket->lock, flags);
			if (new_entry)
				kmem_cache_free(uid_io_cache, new_entry);
			return;
		}
	}

	if (!new_entry) {
		spin_unlock_irqrestore(&bucket->lock, flags);
		new_entry = kmem_cache_zalloc(uid_io_cache, GFP_ATOMIC);
		if (!new_entry)
			return;
		new_entry->uid = uid;
		goto retry;
	}

	if (io_op_is_write(op))
		new_entry->write_kb = kb;
	else
		new_entry->read_kb = kb;
	hlist_add_head(&new_entry->node, &bucket->hash[idx]);
	spin_unlock_irqrestore(&bucket->lock, flags);
}

static bool is_hot_inode(u64 inode_hash)
{
	unsigned long flags;
	int i;
	bool found = false;

	if (!inode_hash)
		return false;

	spin_lock_irqsave(&hot_inode_lock, flags);
	for (i = 0; i < hot_inode_count; i++) {
		if (hot_inodes[i] == inode_hash) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&hot_inode_lock, flags);
	return found;
}

static void inode_curr_add(u64 inode_hash)
{
	struct inode_bucket *bucket;
	struct inode_count_entry *entry;
	struct inode_count_entry *new_entry = NULL;
	unsigned int idx = ino_hash_idx(inode_hash);
	unsigned long flags;

	if (!inode_hash)
		return;

retry:
	bucket = this_cpu_ptr(&inode_curr);
	spin_lock_irqsave(&bucket->lock, flags);
	hlist_for_each_entry(entry, &bucket->hash[idx], node) {
		if (entry->ino == inode_hash) {
			entry->count++;
			spin_unlock_irqrestore(&bucket->lock, flags);
			if (new_entry)
				kmem_cache_free(inode_cache, new_entry);
			return;
		}
	}

	if (!new_entry) {
		spin_unlock_irqrestore(&bucket->lock, flags);
		new_entry = kmem_cache_zalloc(inode_cache, GFP_ATOMIC);
		if (!new_entry)
			return;
		new_entry->ino = inode_hash;
		goto retry;
	}

	new_entry->count = 1;
	hlist_add_head(&new_entry->node, &bucket->hash[idx]);
	spin_unlock_irqrestore(&bucket->lock, flags);
}

static u8 snapshot_device_util(void)
{
	unsigned long flags;
	u64 now = ktime_get_boottime_ns();
	u64 elapsed;
	u64 busy;
	u8 pct = 0;

	if (raw_mode_enabled())
		return 0;

	spin_lock_irqsave(&util_lock, flags);
	elapsed = now - util_window_start;
	busy = busy_ns;
	if (atomic_read(&in_flight) > 0 && ts_busy_start)
		busy += now - ts_busy_start;
	if (elapsed)
		pct = (u8)min_t(u64, 100, div64_u64(busy * 100, elapsed));
	spin_unlock_irqrestore(&util_lock, flags);
	return pct;
}

static void device_issue_account(void)
{
	unsigned long flags;

	spin_lock_irqsave(&util_lock, flags);
	if (atomic_inc_return(&in_flight) == 1)
		ts_busy_start = ktime_get_boottime_ns();
	spin_unlock_irqrestore(&util_lock, flags);
}

static void device_complete_account(void)
{
	unsigned long flags;
	u64 now;

	spin_lock_irqsave(&util_lock, flags);
	if (atomic_read(&in_flight) > 0 && atomic_dec_return(&in_flight) == 0) {
		now = ktime_get_boottime_ns();
		if (ts_busy_start && now > ts_busy_start)
			busy_ns += now - ts_busy_start;
		ts_busy_start = 0;
	}
	spin_unlock_irqrestore(&util_lock, flags);
}

static u32 read_loadavg_1m_x100(void)
{
	return READ_ONCE(cached_loadavg_1m_x100);
}

static u32 calc_queue_sat(u32 flight)
{
	if (!queue_depth)
		return 0;

	return flight * 100 / queue_depth;
}

void set_loadavg_1m_x100(u32 loadavg_1m_x100)
{
	WRITE_ONCE(cached_loadavg_1m_x100, loadavg_1m_x100);
}

void set_psi_io_x100(u32 psi_io_x100)
{
	WRITE_ONCE(cached_psi_io, psi_io_x100);
	if (raw_mode_enabled())
		smart_io_raw_emit("android_vh_psi_group: ts=%llu io_some_avg10_x100=%u\n",
				  ktime_get_boottime_ns(), psi_io_x100);
}

void set_render_avg_lat(u32 render_avg_lat)
{
	WRITE_ONCE(cached_render_avg_lat, render_avg_lat);
}

u32 get_render_avg_lat(void)
{
	return READ_ONCE(cached_render_avg_lat);
}

void set_jank_cnt(u32 jank_cnt)
{
	WRITE_ONCE(cached_jank_cnt, jank_cnt);
}

u32 get_jank_cnt(void)
{
	return READ_ONCE(cached_jank_cnt);
}


void f2fs_gc_begin(void)
{
	atomic_inc(&f2fs_gc_active);
	if (raw_mode_enabled())
		smart_io_raw_emit("f2fs_gc_begin: ts=%llu\n",
				  ktime_get_boottime_ns());
}

void f2fs_gc_end(void)
{
	if (atomic_read(&f2fs_gc_active) > 0)
		atomic_dec(&f2fs_gc_active);
	if (raw_mode_enabled())
		smart_io_raw_emit("f2fs_gc_end: ts=%llu\n",
				  ktime_get_boottime_ns());
}

void f2fs_cp_begin(void)
{
	atomic_inc(&f2fs_cp_active);
	if (raw_mode_enabled())
		smart_io_raw_emit("f2fs_cp_begin: ts=%llu\n",
				  ktime_get_boottime_ns());
}

void f2fs_cp_end(void)
{
	if (atomic_read(&f2fs_cp_active) > 0)
		atomic_dec(&f2fs_cp_active);
	if (raw_mode_enabled())
		smart_io_raw_emit("f2fs_cp_end: ts=%llu\n",
				  ktime_get_boottime_ns());
}

static void emit_raw_insert(const struct smart_io_event *evt,
				     struct request *rq)
{
	u32 nr_segment = get_nr_segment(rq);
	int tag = get_rq_tag(rq);
	u32 tag_depth_max = get_rq_tag_depth_max(rq);
	int tag_depth_cur = get_rq_tag_depth_cur(rq);

	smart_io_raw_emit("block_rq_insert: "
			  "ts_insert=%llu uid=%u pid=%d tgid=%d rq_p=%p io_op=%u io_size=%u cmd=%u "
			  "sector=%llu ioprio=%u file_ext=%u file_ext_s=%s "
			  "fs_type=%s dev_name=%s remap_from=%s remap_mixed=%u dev_type=%u is_sync=%u "
			  "inode_hash=%llu folio_index=%llu io_inflight=%u queue_sat=%u "
			  "nr_segment=%u tag=%d tag_depth_max=%u tag_depth_cur=%d\n",
			  evt->ts_insert, evt->uid, current->pid, current->tgid, (void *)rq,
			  evt->io_op, blk_rq_bytes(rq), rq->cmd_flags, evt->sector, evt->ioprio_class,
			  evt->file_ext, evt->file_ext_str, evt->fs_type,
			  evt->dev_name, evt->remap_from, evt->remap_mixed,
			  evt->device_type, evt->is_sync, evt->inode_hash,
			  evt->folio_index, evt->io_in_flight, evt->queue_saturate,
			  nr_segment, tag, tag_depth_max, tag_depth_cur);
}

static void emit_raw_issue(const struct smart_io_event *evt,
				    struct request *rq, bool new_record)
{
	u32 nr_bio = get_nr_bio(rq);
	u32 nr_segment = get_nr_segment(rq);
	int tag = get_rq_tag(rq);
	u32 tag_depth_max = get_rq_tag_depth_max(rq);
	int tag_depth_cur = get_rq_tag_depth_cur(rq);

	if (new_record) {
		smart_io_raw_emit("block_rq_issue: "
				  "ts_issue=%llu uid=%u pid=%d tgid=%d rq_p=%p io_op=%u io_size=%u cmd=%u "
				  "sector=%llu ioprio=%u file_ext=%u file_ext_s=%s "
				  "fs_type=%s dev_name=%s remap_from=%s remap_mixed=%u dev_type=%u is_sync=%u "
				  "inode_hash=%llu folio_index=%llu io_inflight=%u queue_sat=%u nr_bio=%u "
				  "nr_segment=%u tag=%d tag_depth_max=%u tag_depth_cur=%d direct=1\n",
				  evt->ts_issue, evt->uid, current->pid, current->tgid,
				  (void *)rq, evt->io_op, blk_rq_bytes(rq), rq->cmd_flags, evt->sector,
				  evt->ioprio_class, evt->file_ext, evt->file_ext_str,
				  evt->fs_type, evt->dev_name, evt->remap_from,
				  evt->remap_mixed, evt->device_type, evt->is_sync,
				  evt->inode_hash, evt->folio_index, evt->io_in_flight,
				  evt->queue_saturate, nr_bio, nr_segment, tag,
				  tag_depth_max, tag_depth_cur);
		return;
	}

	smart_io_raw_emit("block_rq_issue: "
			  "ts_issue=%llu uid=%u inode_hash=%llu rq_p=%p io_op=%u remap_from=%s remap_mixed=%u "
			  "nr_bio=%u nr_segment=%u tag=%d tag_depth_max=%u tag_depth_cur=%d\n",
			  evt->ts_issue, evt->uid, evt->inode_hash, (void *)rq,
			  evt->io_op, evt->remap_from, evt->remap_mixed, nr_bio,
			  nr_segment, tag, tag_depth_max, tag_depth_cur);
}

static void emit_raw_requeue(const struct smart_io_event *evt,
				      struct request *rq)
{
	u32 nr_bio = get_nr_bio(rq);
	u32 nr_segment = get_nr_segment(rq);
	int tag = get_rq_tag(rq);
	u32 tag_depth_max = get_rq_tag_depth_max(rq);
	int tag_depth_cur = get_rq_tag_depth_cur(rq);

	smart_io_raw_emit("block_rq_requeue: "
			  "ts_requeue=%llu uid=%u inode_hash=%llu rq_p=%p io_op=%u io_size=%u cmd=%u "
			  "remap_from=%s remap_mixed=%u nr_bio=%u nr_segment=%u tag=%d "
			  "tag_depth_max=%u tag_depth_cur=%d\n",
			  ktime_get_boottime_ns(), evt->uid, evt->inode_hash,
			  (void *)rq, evt->io_op, blk_rq_bytes(rq), rq->cmd_flags,
			  evt->remap_from, evt->remap_mixed, nr_bio, nr_segment,
			  tag, tag_depth_max, tag_depth_cur);
}

static void emit_raw_complete(const struct smart_io_event *evt,
				       struct request *rq,
				       unsigned int nr_bytes)
{
	smart_io_raw_emit("block_rq_complete: "
			  "ts_complete=%llu uid=%u inode_hash=%llu rq_p=%p io_op=%u nr_bytes=%u remap_from=%s remap_mixed=%u\n",
			  evt->ts_complete, evt->uid, evt->inode_hash, (void *)rq,
			  evt->io_op, nr_bytes, evt->remap_from, evt->remap_mixed);
}

static void fill_record_from_current(struct rq_record *rec, struct request *rq,
				     bool direct_path,
				     const struct remap_info *remap)
{
	struct smart_io_event *evt = &rec->event;
	struct bio *bio = rq->bio;
	uid_t fg_uid;
	u32 scene;
	char comm[TASK_COMM_LEN];
	u32 flight = (u32)atomic_read(&in_flight);

	memset(evt, 0, sizeof(*evt));
	smart_io_get_fg_ctx(&fg_uid, &scene);
	get_task_comm(comm, current);
	strscpy(evt->fs_type, "unknown", sizeof(evt->fs_type));
	strscpy(evt->file_ext_str, "unknown", sizeof(evt->file_ext_str));
	strscpy(evt->dev_name, "unknown", sizeof(evt->dev_name));
	strscpy(evt->remap_from, "none", sizeof(evt->remap_from));

	evt->ts_insert = ktime_get_boottime_ns();
	evt->uid = from_kuid(&init_user_ns, current_uid());
	evt->scene_tag = scene;
	evt->is_front = evt->uid == fg_uid;
	evt->thread_role = match_thread_role(evt->uid, current->pid, current->tgid, comm, fg_uid);
	evt->direct_path = direct_path ? 1 : 0;
	evt->io_op = encode_io_op(req_op(rq));
	rec->data_bytes = blk_rq_bytes(rq);
	evt->io_size_kb = blk_rq_bytes(rq) >> 10;
	evt->sector = blk_rq_pos(rq);
	evt->ioprio_class = rq->ioprio;

	extract_device_name(rq, evt->dev_name, sizeof(evt->dev_name));
	apply_remap_info(evt, remap);
	evt->device_type = lookup_device_type(evt->dev_name);
	
	if (bio) {
		bool got_vfs_info;

		evt->is_sync = !!(bio->bi_opf & REQ_SYNC);
		got_vfs_info = extract_vfs_info(bio, &evt->inode_hash,
						&evt->file_ext,
						&evt->folio_index,
						evt->file_ext_str,
						sizeof(evt->file_ext_str),
						evt->fs_type,
						sizeof(evt->fs_type));

		if (!got_vfs_info && get_nr_bio(rq) > 1) {
			struct bio *b;

			__rq_for_each_bio(b, rq) {
				if (b == bio)
					continue;

				got_vfs_info = extract_vfs_info(b,
							      &evt->inode_hash,
							      &evt->file_ext,
							      &evt->folio_index,
							      evt->file_ext_str,
							      sizeof(evt->file_ext_str),
							      evt->fs_type,
							      sizeof(evt->fs_type));
				if (got_vfs_info)
					break;
			}
		}
	}
	evt->queue_saturate = calc_queue_sat(flight);
	evt->io_in_flight = flight;

	if (raw_mode_enabled())
		return;

	evt->cgroup_io_weight = get_cgroup_weight(rq);
	evt->f2fs_gc_active = atomic_read(&f2fs_gc_active) ? 1 : 0;
	evt->f2fs_cp_active = atomic_read(&f2fs_cp_active) ? 1 : 0;
	evt->is_hot_file = is_hot_inode(evt->inode_hash) ? 1 : 0;
	evt->mem_avail_mb = pages_to_mb(si_mem_available());
	evt->dirty_pages_mb = pages_to_mb(global_node_page_state(NR_FILE_DIRTY));
	evt->wb_pages_mb = pages_to_mb(global_node_page_state(NR_WRITEBACK));
	evt->swap_mb = get_swap_used_mb();
	evt->cpu_loadavg_1m = read_loadavg_1m_x100();
	evt->online_cpus = (u8)min_t(unsigned int, num_online_cpus(), U8_MAX);
	evt->runqueue_len = get_runqueue_len();
	evt->device_util_pct = snapshot_device_util();
	evt->insert_cpuid = smp_processor_id();
	evt->io_bw_kbps = uid_io_prev_bandwidth_kbps(evt->uid);
}

static bool should_capture_rq(struct request *rq, u32 *uid_out, bool *sync_out,
			      u32 *scene_out)
{
	struct bio *bio = rq ? rq->bio : NULL;
	uid_t fg_uid;
	u32 scene;
	u32 uid;
	bool sync = false;

	if (!rq)
		return false;

	uid = from_kuid(&init_user_ns, current_uid());
	if (bio)
		sync = !!(bio->bi_opf & REQ_SYNC);
	smart_io_get_fg_ctx(&fg_uid, &scene);
	(void)fg_uid;

	if (!policy_should_capture(uid, sync, scene))
		return false;

	*uid_out = uid;
	*sync_out = sync;
	*scene_out = scene;
	return true;
}

void smart_io_record_bio_remap(struct bio *bio, dev_t old_dev, sector_t old_sector)
{
	struct bio_remap_entry *entry;
	struct bio_remap_entry *new_entry = NULL;
	unsigned long flags;

	(void)old_sector;

	if (!atomic_read(&smart_io_enabled) || !bio || !bio_remap_cache)
		return;

retry:
	spin_lock_irqsave(&bio_remap_lock, flags);
	entry = bio_remap_find_locked(bio);
	if (entry) {
		spin_unlock_irqrestore(&bio_remap_lock, flags);
		if (new_entry)
			kmem_cache_free(bio_remap_cache, new_entry);
		return;
	}

	if (!new_entry) {
		spin_unlock_irqrestore(&bio_remap_lock, flags);
		new_entry = kmem_cache_zalloc(bio_remap_cache, GFP_ATOMIC);
		if (!new_entry)
			return;
		new_entry->bio = bio;
		new_entry->old_dev = old_dev;
		goto retry;
	}

	hash_add(bio_remap_hash, &new_entry->node, (unsigned long)bio);
	spin_unlock_irqrestore(&bio_remap_lock, flags);
}

void smart_io_record_rq_remap(struct request *rq, dev_t old_dev, sector_t old_sector)
{
	struct rq_remap_entry *entry;
	struct rq_remap_entry *new_entry = NULL;
	unsigned long flags;

	(void)old_sector;

	if (!atomic_read(&smart_io_enabled) || !rq || !rq_remap_cache)
		return;

retry:
	spin_lock_irqsave(&rq_remap_lock, flags);
	entry = rq_remap_find_locked(rq);
	if (entry) {
		spin_unlock_irqrestore(&rq_remap_lock, flags);
		if (new_entry)
			kmem_cache_free(rq_remap_cache, new_entry);
		return;
	}

	if (!new_entry) {
		spin_unlock_irqrestore(&rq_remap_lock, flags);
		new_entry = kmem_cache_zalloc(rq_remap_cache, GFP_ATOMIC);
		if (!new_entry)
			return;
		new_entry->rq = rq;
		new_entry->old_dev = old_dev;
		goto retry;
	}

	hash_add(rq_remap_hash, &new_entry->node, (unsigned long)rq);
	spin_unlock_irqrestore(&rq_remap_lock, flags);
}

void smart_io_record_insert(struct request *rq)
{
	struct rq_record *rec;
	struct rq_record *old;
	struct remap_info remap;
	struct rq_shard *shard;
	unsigned long flags;
	u32 uid;
	u32 scene;
	bool sync;

	if (!atomic_read(&smart_io_enabled) || !rq)
		return;
	consume_request_remap(rq, &remap);
	if (!should_capture_rq(rq, &uid, &sync, &scene))
		return;

	rec = kmem_cache_zalloc(rq_record_cache, GFP_ATOMIC);
	if (!rec)
		return;

	rec->rq = rq;
	fill_record_from_current(rec, rq, false, &remap);
	if (!raw_mode_enabled()) {
		uid_io_curr_add(rec->event.uid, rec->event.io_size_kb,
				rec->event.io_op);
		inode_curr_add(rec->event.inode_hash);
	}

	shard = &rq_shards[rq_shard_idx(rq)];
	spin_lock_irqsave(&shard->lock, flags);
	old = rq_find_locked(shard, rq);
	if (old) {
		hash_del(&old->node);
		smart_io_trace_debug("insert: duplicate request %p,\n"
			"new | old\n"
			"ts_insert: %llu | %llu\n"
			"issued: %u | %u\n"
			"uid: %u | %u\n"
			"io_op: %u | %u\n"
			"io_size_kb: %u | %u\n"
			"sector: %llu | %llu\n"
			"ioprio: %u | %u\n"
			"file_ext: %u | %u\n"
			"inode_hash: %llu | %llu\n",
			rq, (unsigned long long)rec->event.ts_insert, (unsigned long long)old->event.ts_insert,
			rec->issued, old->issued,
			(unsigned)rec->event.uid, (unsigned)old->event.uid,
			rec->event.io_op, old->event.io_op,
			rec->event.io_size_kb, old->event.io_size_kb,
			rec->event.sector, old->event.sector,
			rec->event.ioprio_class, old->event.ioprio_class,
			rec->event.file_ext, old->event.file_ext,
			rec->event.inode_hash, old->event.inode_hash);
		kmem_cache_free(rq_record_cache, old);
	}
	hash_add(shard->hash, &rec->node, (unsigned long)rq);
	spin_unlock_irqrestore(&shard->lock, flags);

	this_cpu_inc(smart_io_cnts.insert_cnt);
	if (raw_mode_enabled())
		emit_raw_insert(&rec->event, rq);
}

void smart_io_record_issue(struct request *rq)
{
	struct rq_record *rec;
	struct remap_info remap;
	struct rq_shard *shard;
	unsigned long flags;
	u32 uid;
	u32 scene;
	bool sync;
	bool new_record = false;

	if (!atomic_read(&smart_io_enabled) || !rq)
		return;

	shard = &rq_shards[rq_shard_idx(rq)];
	spin_lock_irqsave(&shard->lock, flags);
	rec = rq_find_locked(shard, rq);
	spin_unlock_irqrestore(&shard->lock, flags);

	if (!rec) {
		consume_request_remap(rq, &remap);
		if (!should_capture_rq(rq, &uid, &sync, &scene))
			return;
		rec = kmem_cache_zalloc(rq_record_cache, GFP_ATOMIC);
		if (!rec)
			return;
		rec->rq = rq;
		fill_record_from_current(rec, rq, true, &remap);
		rec->event.ts_issue = rec->event.ts_insert;
		if (!raw_mode_enabled()) {
			uid_io_curr_add(rec->event.uid, rec->event.io_size_kb,
					rec->event.io_op);
			inode_curr_add(rec->event.inode_hash);
		}
		new_record = true;
	}

	spin_lock_irqsave(&shard->lock, flags);
	if (new_record) {
		hash_add(shard->hash, &rec->node, (unsigned long)rq);
		this_cpu_inc(smart_io_cnts.direct_issue_cnt);
	} else {
		rec = rq_find_locked(shard, rq);
		if (!rec) {
			smart_io_trace_debug("issue: lost request %p\n", rq);
			spin_unlock_irqrestore(&shard->lock, flags);
			device_complete_account();
			return;
		}
		rec->event.ts_issue = ktime_get_boottime_ns();
	}

	if (!rec->issued) {
		rec->issued = true;
		device_issue_account();
	}
	rec->event.issue_cpuid = smp_processor_id();
	spin_unlock_irqrestore(&shard->lock, flags);

	this_cpu_inc(smart_io_cnts.issue_cnt);
	if (raw_mode_enabled()) {
		emit_raw_issue(&rec->event, rq, new_record);
	}
}

void smart_io_record_requeue(struct request *rq)
{
	struct rq_record *rec;
	struct smart_io_event evt;
	struct rq_shard *shard;
	unsigned long flags;

	if (!atomic_read(&smart_io_enabled) || !rq || !raw_mode_enabled())
		return;

	shard = &rq_shards[rq_shard_idx(rq)];
	spin_lock_irqsave(&shard->lock, flags);
	rec = rq_find_locked(shard, rq);
	if (!rec) {
		spin_unlock_irqrestore(&shard->lock, flags);
		return;
	}
	evt = rec->event;
	spin_unlock_irqrestore(&shard->lock, flags);

	emit_raw_requeue(&evt, rq);
}



static void hist_add(struct lat_hist *hist, u64 latency_us)
{
	u8 idx = lat_bucket_idx(latency_us ? latency_us : 1, HIST_BUCKETS);

	hist->count[idx]++;
	hist->sum_us += latency_us;
	hist->total++;
}

static u32 hist_percentile(const struct lat_hist *hist, u32 pct)
{
	u64 target;
	u64 seen = 0;
	u8 i;

	if (!hist->total)
		return 0;
	target = div64_u64(hist->total * pct + 99, 100);
	for (i = 0; i < HIST_BUCKETS; i++) {
		seen += hist->count[i];
		if (seen >= target)
			return lat_bucket_value(i);
	}
	return lat_bucket_value(HIST_BUCKETS - 1);
}

static void cur_bucket_add(u8 op, u32 kb, u64 latency_us)
{
	struct percpu_bucket *bucket;
	unsigned long flags;

	bucket = this_cpu_ptr(&cur_bucket);
	spin_lock_irqsave(&bucket->lock, flags);
	if (io_op_is_write(op)) {
		hist_add(&bucket->write_hist, latency_us);
		bucket->write_kb += kb;
	} else {
		hist_add(&bucket->read_hist, latency_us);
		bucket->read_kb += kb;
	}
	bucket->io_count++;
	bucket->byte_kb += kb;
	spin_unlock_irqrestore(&bucket->lock, flags);
}

static void cur_bucket_filemap_add(u32 access_pages, u32 miss_pages,
				       u32 cache_add_pages, u32 cache_delete_pages,
				       u32 refault_pages, u32 fault_count,
				       u64 fault_wait_us)
{
	struct percpu_bucket *bucket;
	unsigned long flags;

	bucket = this_cpu_ptr(&cur_bucket);
	spin_lock_irqsave(&bucket->lock, flags);
	bucket->filemap_read_pages += access_pages;
	bucket->filemap_miss_pages += miss_pages;
	bucket->filemap_cache_add_pages += cache_add_pages;
	bucket->filemap_cache_delete_pages += cache_delete_pages;
	bucket->filemap_refault_pages += refault_pages;
	bucket->filemap_fault_count += fault_count;
	bucket->filemap_fault_wait_us += fault_wait_us;
	spin_unlock_irqrestore(&bucket->lock, flags);
}

void smart_io_record_filemap_pages(u32 total_pages)
{
	if (!atomic_read(&smart_io_enabled) || !total_pages || raw_mode_enabled())
		return;

	cur_bucket_filemap_add(total_pages, 0, 0, 0, 0, 0, 0);
}

void smart_io_record_filemap_miss(u32 miss_pages)
{
	if (!atomic_read(&smart_io_enabled) || !miss_pages || raw_mode_enabled())
		return;

	cur_bucket_filemap_add(0, miss_pages, 0, 0, 0, 0, 0);
}

void smart_io_record_filemap_cache_add(u32 pages)
{
	if (!atomic_read(&smart_io_enabled) || !pages || raw_mode_enabled())
		return;

	cur_bucket_filemap_add(0, 0, pages, 0, 0, 0, 0);
}

void smart_io_record_filemap_cache_delete(u32 pages)
{
	if (!atomic_read(&smart_io_enabled) || !pages || raw_mode_enabled())
		return;

	cur_bucket_filemap_add(0, 0, 0, pages, 0, 0, 0);
}

void smart_io_record_filemap_refault(u32 pages)
{
	if (!atomic_read(&smart_io_enabled) || !pages || raw_mode_enabled())
		return;

	cur_bucket_filemap_add(0, 0, 0, 0, pages, 0, 0);
}

void smart_io_record_filemap_fault(u64 wait_us)
{
	if (!atomic_read(&smart_io_enabled) || raw_mode_enabled())
		return;

	cur_bucket_filemap_add(0, 0, 0, 0, 0, 1, wait_us);
}

static void copy_last_bucket_fields(struct smart_io_event *evt)
{
	unsigned long flags;

	spin_lock_irqsave(&last_bucket_lock, flags);
	evt->p50_read_lat_us = last_bucket.p50_rlat;
	evt->p50_write_lat_us = last_bucket.p50_wlat;
	evt->p99_read_lat_us = last_bucket.p99_rlat;
	evt->p99_write_lat_us = last_bucket.p99_wlat;
	evt->avg_read_lat_us = last_bucket.avg_rlat;
	evt->avg_write_lat_us = last_bucket.avg_wlat;
	evt->p50_p99_ratio_r = last_bucket.ratio_r;
	evt->p50_p99_ratio_w = last_bucket.ratio_w;
	evt->reclaim_pages = last_bucket.reclaim_diff;
	evt->blk_bw_util = last_bucket.blk_bw_util;
	evt->cache_hit = last_bucket.page_cache_hit_pct;
	evt->file_access_pages = last_bucket.file_access_pages;
	evt->file_miss_pages = last_bucket.file_miss_pages;
	evt->file_cache_add_pages = last_bucket.file_cache_add_pages;
	evt->file_cache_delete_pages = last_bucket.file_cache_delete_pages;
	evt->file_refault_pages = last_bucket.file_refault_pages;
	evt->file_fault_count = last_bucket.file_fault_count;
	evt->file_fault_wait_us = last_bucket.file_fault_wait_us;
	spin_unlock_irqrestore(&last_bucket_lock, flags);
	evt->sys_psi_io = READ_ONCE(cached_psi_io);
	evt->render_avg_lat = READ_ONCE(cached_render_avg_lat);
	evt->jank_cnt = READ_ONCE(cached_jank_cnt);
}

void smart_io_record_complete(struct request *rq, unsigned int nr_bytes)
{
	struct rq_record *rec;
	struct smart_io_event evt;
	struct rq_shard *shard;
	unsigned long flags;
	u64 sched_us;
	u64 dev_us;

	(void)nr_bytes;

	if (!atomic_read(&smart_io_enabled) || !rq)
		return;

	shard = &rq_shards[rq_shard_idx(rq)];
	spin_lock_irqsave(&shard->lock, flags);
	rec = rq_find_locked(shard, rq);
	if (!rec) {
		spin_unlock_irqrestore(&shard->lock, flags);
		this_cpu_inc(smart_io_cnts.lost_complete_cnt);
		if (nr_bytes > 0)
			smart_io_trace_debug("complete: lost request %p nr_bytes=%u\n", rq, nr_bytes);
		return;
	}
	spin_unlock_irqrestore(&shard->lock, flags);

	evt = rec->event;
	evt.ts_complete = ktime_get_boottime_ns();

	rec->data_bytes -= nr_bytes;
	if (rec->issued)
		device_complete_account();

	if (raw_mode_enabled()) {
		emit_raw_complete(&evt, rq, nr_bytes);
		this_cpu_inc(smart_io_cnts.complete_cnt);
		if (rec->data_bytes <= 0)
			goto cleanup;
		return;
	}

	if (!evt.ts_issue)
		evt.ts_issue = evt.ts_insert;
	if (evt.ts_complete > evt.ts_insert)
		evt.latency_us = div64_u64(evt.ts_complete - evt.ts_insert, 1000);
	if (evt.ts_issue > evt.ts_insert)
		sched_us = div64_u64(evt.ts_issue - evt.ts_insert, 1000);
	else
		sched_us = 0;
	if (evt.ts_complete > evt.ts_issue)
		dev_us = div64_u64(evt.ts_complete - evt.ts_issue, 1000);
	else
		dev_us = 0;
	evt.sched_us = sched_us;
	evt.dev_us = dev_us;
	if (evt.latency_us)
		evt.sched_pct = (u32)div64_u64(sched_us * 100, evt.latency_us);

	cur_bucket_add(evt.io_op, evt.io_size_kb, evt.latency_us);
	copy_last_bucket_fields(&evt);
	// aggregate_cur_bucket(&evt.cur_bucket_ios, &evt.cur_bucket_kb);

	spin_lock_irqsave(&latest_event_lock, flags);
	latest_event = evt;
	spin_unlock_irqrestore(&latest_event_lock, flags);

	smart_io_trace_emit_complete(&evt);
	this_cpu_inc(smart_io_cnts.complete_cnt);

	if (rec->data_bytes > 0)
		return;

cleanup:
	spin_lock_irqsave(&shard->lock, flags);
	hash_del(&rec->node);
	spin_unlock_irqrestore(&shard->lock, flags);
	kmem_cache_free(rq_record_cache, rec);
}

static void free_uid_prev_locked(void)
{
	struct uid_io_entry *entry;
	struct hlist_node *tmp;
	int i;

	for (i = 0; i < ARRAY_SIZE(uid_io_prev); i++) {
		hlist_for_each_entry_safe(entry, tmp, &uid_io_prev[i], node) {
			hlist_del(&entry->node);
			kmem_cache_free(uid_io_cache, entry);
		}
		INIT_HLIST_HEAD(&uid_io_prev[i]);
	}
}

static struct uid_io_entry *uid_prev_find_locked(u32 uid)
{
	struct uid_io_entry *entry;
	unsigned int idx = uid_hash_idx(uid);

	hlist_for_each_entry(entry, &uid_io_prev[idx], node) {
		if (entry->uid == uid)
			return entry;
	}
	return NULL;
}

static void uid_io_swap_buckets(void)
{
	struct uid_io_bucket *bucket;
	struct uid_io_entry *entry;
	struct hlist_node *tmp;
	struct uid_io_entry *prev;
	unsigned long flags;
	unsigned long prev_flags;
	int cpu;
	int i;

	write_lock_irqsave(&uid_io_prev_lock, prev_flags);
	free_uid_prev_locked();

	for_each_possible_cpu(cpu) {
		bucket = per_cpu_ptr(&uid_io_curr, cpu);
		spin_lock_irqsave(&bucket->lock, flags);
		for (i = 0; i < ARRAY_SIZE(bucket->hash); i++) {
			hlist_for_each_entry_safe(entry, tmp, &bucket->hash[i], node) {
				hlist_del(&entry->node);
				prev = uid_prev_find_locked(entry->uid);
				if (prev) {
					prev->read_kb += entry->read_kb;
					prev->write_kb += entry->write_kb;
					kmem_cache_free(uid_io_cache, entry);
				} else {
					hlist_add_head(&entry->node,
						       &uid_io_prev[uid_hash_idx(entry->uid)]);
				}
			}
		}
		spin_unlock_irqrestore(&bucket->lock, flags);
	}
	write_unlock_irqrestore(&uid_io_prev_lock, prev_flags);
}

static void hot_inode_consider(u64 ino, u64 count, u64 *top_ino, u64 *top_count)
{
	int i;
	int min_idx = 0;

	if (!ino || !count)
		return;

	for (i = 0; i < HOT_INODE_MAX; i++) {
		if (!top_ino[i]) {
			top_ino[i] = ino;
			top_count[i] = count;
			return;
		}
		if (top_ino[i] == ino) {
			top_count[i] += count;
			return;
		}
		if (top_count[i] < top_count[min_idx])
			min_idx = i;
	}

	if (count > top_count[min_idx]) {
		top_ino[min_idx] = ino;
		top_count[min_idx] = count;
	}
}

static void roll_hot_inodes(void)
{
	struct inode_bucket *bucket;
	struct inode_count_entry *entry;
	struct hlist_node *tmp;
	u64 top_ino[HOT_INODE_MAX] = { 0 };
	u64 top_count[HOT_INODE_MAX] = { 0 };
	unsigned long flags;
	int cpu;
	int i;
	int n = 0;

	for_each_possible_cpu(cpu) {
		bucket = per_cpu_ptr(&inode_curr, cpu);
		spin_lock_irqsave(&bucket->lock, flags);
		for (i = 0; i < ARRAY_SIZE(bucket->hash); i++) {
			hlist_for_each_entry_safe(entry, tmp, &bucket->hash[i], node) {
				hot_inode_consider(entry->ino, entry->count, top_ino, top_count);
				hlist_del(&entry->node);
				kmem_cache_free(inode_cache, entry);
			}
		}
		spin_unlock_irqrestore(&bucket->lock, flags);
	}

	spin_lock_irqsave(&hot_inode_lock, flags);
	for (i = 0; i < HOT_INODE_MAX; i++) {
		if (top_ino[i])
			hot_inodes[n++] = top_ino[i];
	}
	hot_inode_count = n;
	spin_unlock_irqrestore(&hot_inode_lock, flags);
}

static void roll_completed_bucket(void)
{
	struct percpu_bucket *bucket;
	struct lat_hist read_hist = { 0 };
	struct lat_hist write_hist = { 0 };
	struct completed_bucket next = { 0 };
	unsigned long flags;
	u64 reclaim_now;
	u64 read_kb = 0;
	u64 write_kb = 0;
	u64 total_kb = 0;
	u64 filemap_total_pages = 0;
	u64 filemap_miss_pages = 0;
	u64 filemap_cache_add_pages = 0;
	u64 filemap_cache_delete_pages = 0;
	u64 filemap_refault_pages = 0;
	u64 filemap_fault_count = 0;
	u64 filemap_fault_wait_us = 0;
	int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		bucket = per_cpu_ptr(&cur_bucket, cpu);
		spin_lock_irqsave(&bucket->lock, flags);
		for (i = 0; i < HIST_BUCKETS; i++) {
			read_hist.count[i] += bucket->read_hist.count[i];
			write_hist.count[i] += bucket->write_hist.count[i];
		}
		read_hist.sum_us += bucket->read_hist.sum_us;
		read_hist.total += bucket->read_hist.total;
		write_hist.sum_us += bucket->write_hist.sum_us;
		write_hist.total += bucket->write_hist.total;
		read_kb += bucket->read_kb;
		write_kb += bucket->write_kb;
		total_kb += bucket->byte_kb;
		memset(&bucket->read_hist, 0, sizeof(bucket->read_hist));
		memset(&bucket->write_hist, 0, sizeof(bucket->write_hist));
		bucket->io_count = 0;
		bucket->byte_kb = 0;
		bucket->read_kb = 0;
		bucket->write_kb = 0;
		filemap_total_pages += bucket->filemap_read_pages;
		filemap_miss_pages += bucket->filemap_miss_pages;
		filemap_cache_add_pages += bucket->filemap_cache_add_pages;
		filemap_cache_delete_pages += bucket->filemap_cache_delete_pages;
		filemap_refault_pages += bucket->filemap_refault_pages;
		filemap_fault_count += bucket->filemap_fault_count;
		filemap_fault_wait_us += bucket->filemap_fault_wait_us;
		bucket->filemap_read_pages = 0;
		bucket->filemap_miss_pages = 0;
		bucket->filemap_cache_add_pages = 0;
		bucket->filemap_cache_delete_pages = 0;
		bucket->filemap_refault_pages = 0;
		bucket->filemap_fault_count = 0;
		bucket->filemap_fault_wait_us = 0;
		spin_unlock_irqrestore(&bucket->lock, flags);
	}

	if (read_hist.total) {
		next.p50_rlat = hist_percentile(&read_hist, 50);
		next.p99_rlat = hist_percentile(&read_hist, 99);
		next.avg_rlat = (u32)div64_u64(read_hist.sum_us, read_hist.total);
		if (next.p99_rlat)
			next.ratio_r = (next.p50_rlat * 100U) / next.p99_rlat;
	}
	if (write_hist.total) {
		next.p50_wlat = hist_percentile(&write_hist, 50);
		next.p99_wlat = hist_percentile(&write_hist, 99);
		next.avg_wlat = (u32)div64_u64(write_hist.sum_us, write_hist.total);
		if (next.p99_wlat)
			next.ratio_w = (next.p50_wlat * 100U) / next.p99_wlat;
	}
	next.burst_total_kbps = kb_window_to_kbps(total_kb, window_sz_ms);
	next.blk_bw_util = calc_blk_bw_util(next.burst_total_kbps);

	reclaim_now = calc_reclaim_pages();
	if (reclaim_now >= prev_reclaim_pages)
		next.reclaim_diff = reclaim_now - prev_reclaim_pages;
	prev_reclaim_pages = reclaim_now;
	next.file_access_pages = filemap_total_pages;
	next.file_miss_pages = filemap_miss_pages;
	next.file_cache_add_pages = filemap_cache_add_pages;
	next.file_cache_delete_pages = filemap_cache_delete_pages;
	next.file_refault_pages = filemap_refault_pages;
	next.file_fault_count = filemap_fault_count;
	next.file_fault_wait_us = filemap_fault_wait_us;
	if (filemap_total_pages) {
		u64 hit_pages;

		if (filemap_miss_pages > filemap_total_pages)
			filemap_miss_pages = filemap_total_pages;
		hit_pages = filemap_total_pages - filemap_miss_pages;
		next.page_cache_hit_pct = (u32)min_t(u64, 100,
				div64_u64(hit_pages * 100, filemap_total_pages));
	}

	spin_lock_irqsave(&last_bucket_lock, flags);
	if (read_hist.total || write_hist.total)
		last_bucket = next;
	else {
		last_bucket.reclaim_diff = next.reclaim_diff;
		last_bucket.blk_bw_util = next.blk_bw_util;
		last_bucket.page_cache_hit_pct = next.page_cache_hit_pct;
		last_bucket.file_access_pages = next.file_access_pages;
		last_bucket.file_miss_pages = next.file_miss_pages;
		last_bucket.file_cache_add_pages = next.file_cache_add_pages;
		last_bucket.file_cache_delete_pages = next.file_cache_delete_pages;
		last_bucket.file_refault_pages = next.file_refault_pages;
		last_bucket.file_fault_count = next.file_fault_count;
		last_bucket.file_fault_wait_us = next.file_fault_wait_us;
	}
	spin_unlock_irqrestore(&last_bucket_lock, flags);

	spin_lock_irqsave(&util_lock, flags);
	busy_ns = 0;
	util_window_start = ktime_get_boottime_ns();
	if (atomic_read(&in_flight) > 0)
		ts_busy_start = util_window_start;
	spin_unlock_irqrestore(&util_lock, flags);
}

void smart_io_periodic_tick(void)
{
	if (raw_mode_enabled()) {
		return;
	}

	uid_io_swap_buckets();
	// if (++tick_count >= ROLL_TICKS) {
	// 	tick_count = 0;
	roll_completed_bucket();
	roll_hot_inodes();
	// }
}

void smart_io_get_io_stats(u32 *insert, u32 *issue, u32 *complete,
			   u32 *waiting, u32 *flight, u32 *lost_complete)
{
	u64 i_cnt = 0;
	u64 is_cnt = 0;
	u64 c_cnt = 0;
	u64 lost_cnt = 0;
	int cpu;

	for_each_online_cpu(cpu) {
		i_cnt += per_cpu(smart_io_cnts, cpu).insert_cnt;
		is_cnt += per_cpu(smart_io_cnts, cpu).issue_cnt;
		c_cnt += per_cpu(smart_io_cnts, cpu).complete_cnt;
		lost_cnt += per_cpu(smart_io_cnts, cpu).lost_complete_cnt;
	}

	*insert = (u32)i_cnt;
	*issue = (u32)is_cnt;
	*complete = (u32)c_cnt;
	*lost_complete = (u32)lost_cnt;
	*waiting = (i_cnt > is_cnt) ? (u32)(i_cnt - is_cnt) : 0;
	*flight = (u32)atomic_read(&in_flight);
}

int smart_io_read_latest_event(struct smart_io_event *dst)
{
	unsigned long flags;

	spin_lock_irqsave(&latest_event_lock, flags);
	*dst = latest_event;
	spin_unlock_irqrestore(&latest_event_lock, flags);
	return 0;
}

static void cleanup_bio_remap_entries(void)
{
	struct bio_remap_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;

	if (!bio_remap_cache)
		return;

	spin_lock_irqsave(&bio_remap_lock, flags);
	hash_for_each_safe(bio_remap_hash, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		kmem_cache_free(bio_remap_cache, entry);
	}
	spin_unlock_irqrestore(&bio_remap_lock, flags);
}

static void cleanup_rq_remap_entries(void)
{
	struct rq_remap_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;

	if (!rq_remap_cache)
		return;

	spin_lock_irqsave(&rq_remap_lock, flags);
	hash_for_each_safe(rq_remap_hash, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		kmem_cache_free(rq_remap_cache, entry);
	}
	spin_unlock_irqrestore(&rq_remap_lock, flags);
}

void smart_io_clear_remap_state(void)
{
	cleanup_bio_remap_entries();
	cleanup_rq_remap_entries();
}

static void cleanup_rq_records(void)
{
	struct rq_record *rec;
	struct hlist_node *tmp;
	int bkt;
	int i;

	for (i = 0; i < NR_RQ_SHARDS; i++) {
		spin_lock(&rq_shards[i].lock);
		hash_for_each_safe(rq_shards[i].hash, bkt, tmp, rec, node) {
			hash_del(&rec->node);
			kmem_cache_free(rq_record_cache, rec);
		}
		spin_unlock(&rq_shards[i].lock);
	}
}

static void cleanup_uid_entries(void)
{
	struct uid_io_bucket *bucket;
	struct uid_io_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	unsigned long prev_flags;
	int cpu;
	int i;

	write_lock_irqsave(&uid_io_prev_lock, prev_flags);
	free_uid_prev_locked();
	write_unlock_irqrestore(&uid_io_prev_lock, prev_flags);

	for_each_possible_cpu(cpu) {
		bucket = per_cpu_ptr(&uid_io_curr, cpu);
		spin_lock_irqsave(&bucket->lock, flags);
		for (i = 0; i < ARRAY_SIZE(bucket->hash); i++) {
			hlist_for_each_entry_safe(entry, tmp, &bucket->hash[i], node) {
				hlist_del(&entry->node);
				kmem_cache_free(uid_io_cache, entry);
			}
		}
		spin_unlock_irqrestore(&bucket->lock, flags);
	}
}

static void cleanup_inode_entries(void)
{
	struct inode_bucket *bucket;
	struct inode_count_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		bucket = per_cpu_ptr(&inode_curr, cpu);
		spin_lock_irqsave(&bucket->lock, flags);
		for (i = 0; i < ARRAY_SIZE(bucket->hash); i++) {
			hlist_for_each_entry_safe(entry, tmp, &bucket->hash[i], node) {
				hlist_del(&entry->node);
				kmem_cache_free(inode_cache, entry);
			}
		}
		spin_unlock_irqrestore(&bucket->lock, flags);
	}
}

int smart_io_semantics_init(void)
{
	struct uid_io_bucket *uid_bucket;
	struct inode_bucket *ino_bucket;
	struct percpu_bucket *bucket;
	int cpu;
	int i;

	rq_record_cache = KMEM_CACHE(rq_record, SLAB_HWCACHE_ALIGN);
	if (!rq_record_cache) {
		smart_io_log_err("failed to allocate rq_record_cache\n");
		return -ENOMEM;
	}
	bio_remap_cache = KMEM_CACHE(bio_remap_entry, SLAB_HWCACHE_ALIGN);
	if (!bio_remap_cache) {
		smart_io_log_err("failed to allocate bio_remap_cache\n");
		goto err_bio_remap;
	}
	rq_remap_cache = KMEM_CACHE(rq_remap_entry, SLAB_HWCACHE_ALIGN);
	if (!rq_remap_cache) {
		smart_io_log_err("failed to allocate rq_remap_cache\n");
		goto err_rq_remap;
	}
	uid_io_cache = KMEM_CACHE(uid_io_entry, SLAB_HWCACHE_ALIGN);
	if (!uid_io_cache) {
		smart_io_log_err("failed to allocate uid_io_cache\n");
		goto err_uid_cache;
	}
	inode_cache = KMEM_CACHE(inode_count_entry, SLAB_HWCACHE_ALIGN);
	if (!inode_cache) {
		smart_io_log_err("failed to allocate inode_cache\n");
		goto err_inode;
	}

	hash_init(bio_remap_hash);
	hash_init(rq_remap_hash);
	for (i = 0; i < NR_RQ_SHARDS; i++) {
		hash_init(rq_shards[i].hash);
		spin_lock_init(&rq_shards[i].lock);
	}
	for (i = 0; i < ARRAY_SIZE(uid_io_prev); i++)
		INIT_HLIST_HEAD(&uid_io_prev[i]);

	for_each_possible_cpu(cpu) {
		uid_bucket = per_cpu_ptr(&uid_io_curr, cpu);
		spin_lock_init(&uid_bucket->lock);
		for (i = 0; i < ARRAY_SIZE(uid_bucket->hash); i++)
			INIT_HLIST_HEAD(&uid_bucket->hash[i]);

		ino_bucket = per_cpu_ptr(&inode_curr, cpu);
		spin_lock_init(&ino_bucket->lock);
		for (i = 0; i < ARRAY_SIZE(ino_bucket->hash); i++)
			INIT_HLIST_HEAD(&ino_bucket->hash[i]);

		bucket = per_cpu_ptr(&cur_bucket, cpu);
		spin_lock_init(&bucket->lock);
		memset(&bucket->read_hist, 0, sizeof(bucket->read_hist));
		memset(&bucket->write_hist, 0, sizeof(bucket->write_hist));
		bucket->io_count = 0;
		bucket->byte_kb = 0;
		bucket->read_kb = 0;
		bucket->write_kb = 0;
		bucket->filemap_read_pages = 0;
		bucket->filemap_miss_pages = 0;
		bucket->filemap_cache_add_pages = 0;
		bucket->filemap_cache_delete_pages = 0;
		bucket->filemap_refault_pages = 0;
		bucket->filemap_fault_count = 0;
		bucket->filemap_fault_wait_us = 0;
	}

	atomic_set(&f2fs_gc_active, 0);
	atomic_set(&f2fs_cp_active, 0);
	prev_reclaim_pages = calc_reclaim_pages();
	util_window_start = ktime_get_boottime_ns();
	smart_io_log_info("semantics initialized: rq_shards=%u blk_max_bw_kbps=%u\n",
			  NR_RQ_SHARDS, blk_max_bw_kbps);
	return 0;

err_inode:
	kmem_cache_destroy(uid_io_cache);
	uid_io_cache = NULL;
err_uid_cache:
	kmem_cache_destroy(rq_remap_cache);
	rq_remap_cache = NULL;
err_rq_remap:
	kmem_cache_destroy(bio_remap_cache);
	bio_remap_cache = NULL;
err_bio_remap:
	kmem_cache_destroy(rq_record_cache);
	rq_record_cache = NULL;
	return -ENOMEM;
}

void smart_io_semantics_exit(void)
{
	smart_io_clear_remap_state();
	cleanup_rq_records();
	cleanup_uid_entries();
	cleanup_inode_entries();

	kmem_cache_destroy(inode_cache);
	kmem_cache_destroy(uid_io_cache);
	kmem_cache_destroy(rq_remap_cache);
	kmem_cache_destroy(bio_remap_cache);
	kmem_cache_destroy(rq_record_cache);
	inode_cache = NULL;
	uid_io_cache = NULL;
	rq_remap_cache = NULL;
	bio_remap_cache = NULL;
	rq_record_cache = NULL;
	smart_io_log_info("semantics released.\n");
}
