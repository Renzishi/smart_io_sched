#ifndef IO_SEMANTICS_H
#define IO_SEMANTICS_H

#include <linux/blk-mq.h>
#include <linux/types.h>

#include "smart_io_types.h"

int smart_io_semantics_init(void);
void smart_io_semantics_exit(void);

void smart_io_record_insert(struct request *rq);
void smart_io_record_issue(struct request *rq);
void smart_io_record_requeue(struct request *rq);
void smart_io_record_complete(struct request *rq, unsigned int nr_bytes);
void smart_io_record_bio_remap(struct bio *bio, dev_t old_dev, sector_t old_sector);
void smart_io_record_rq_remap(struct request *rq, dev_t old_dev, sector_t old_sector);
void smart_io_record_filemap_pages(u32 total_pages);
void smart_io_record_filemap_miss(u32 miss_pages);
void smart_io_record_filemap_cache_add(u32 pages);
void smart_io_record_filemap_cache_delete(u32 pages);
void smart_io_record_filemap_refault(u32 pages);
void smart_io_record_filemap_fault(u64 wait_us);
void smart_io_periodic_tick(void);
void set_loadavg_1m_x100(u32 loadavg_1m_x100);
void set_psi_io_x100(u32 psi_io_x100);
u32 get_render_avg_lat(void);
void set_render_avg_lat(u32 render_avg_lat);
u32 get_jank_cnt(void);
void set_jank_cnt(u32 jank_cnt);
u32 get_window_sz_ms(void);
void set_window_sz_ms(u32 sz_ms);

void f2fs_gc_begin(void);
void f2fs_gc_end(void);
void f2fs_cp_begin(void);
void f2fs_cp_end(void);
void smart_io_clear_remap_state(void);

void smart_io_get_io_stats(u32 *insert, u32 *issue, u32 *complete,
			   u32 *waiting, u32 *in_flight, u32 *lost_complete);
int smart_io_read_latest_event(struct smart_io_event *dst);

#endif
