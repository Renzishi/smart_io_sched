#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/trace.h>

#include "smart_io_log.h"
#include "smart_io_types.h"
#include "trace_instance.h"

#define SMART_IO_TRACE_INSTANCE_NAME "smart_io_sched"

atomic_t rawdata_trace_enabled = ATOMIC_INIT(0);
atomic_t debug_enabled = ATOMIC_INIT(0);

static char smart_io_trace_instance_name[32] = SMART_IO_TRACE_INSTANCE_NAME;

static struct trace_array *smart_io_trace_array;

static struct trace_array *instance_retry(void)
{
	struct trace_array *tr;
	int ret;

	tr = trace_array_get_by_name(smart_io_trace_instance_name, NULL);
	if (!tr) {
		smart_io_log_err("retry: failed to get tracing instance: %s \n",
				 smart_io_trace_instance_name);
		return NULL;
	}

	ret = trace_array_init_printk(tr);
	if (ret) {
		smart_io_log_err("trace_array_init_printk failed for %s: %d\n",
				 smart_io_trace_instance_name, ret);
		trace_array_put(tr);
		return NULL;
	}
	WRITE_ONCE(smart_io_trace_array, tr);
	smart_io_log_info("retry: tracing instance is now ready: %s\n",
			  smart_io_trace_instance_name);
	return tr;
}

int smart_io_trace_instance_init(void)
{
	struct trace_array *tr;
	int ret;

	tr = trace_array_get_by_name(smart_io_trace_instance_name, NULL);
	if (unlikely(!tr)) {
		smart_io_log_err("failed to create tracing instance: %s\n",
				 smart_io_trace_instance_name);
		return -EAGAIN;
	}

	ret = trace_array_init_printk(tr);
	if (ret) {
		smart_io_log_err("trace_array_init_printk failed for %s: %d\n",
				 smart_io_trace_instance_name, ret);
		trace_array_put(tr);
		return ret;
	}

	WRITE_ONCE(smart_io_trace_array, tr);
	smart_io_log_info("tracing instance ready: %s\n",
			  smart_io_trace_instance_name);
	return 0;
}

void smart_io_trace_instance_exit(void)
{
	struct trace_array *tr = READ_ONCE(smart_io_trace_array);

	WRITE_ONCE(smart_io_trace_array, NULL);
	if (!tr)
		return;

	trace_array_put(tr);
	smart_io_log_info("tracing instance released: %s\n",
			  smart_io_trace_instance_name);
}

void smart_io_trace_emit_complete(const struct smart_io_event *evt)
{
	struct trace_array *tr = READ_ONCE(smart_io_trace_array);

	if (unlikely(!tr || !evt))
		return;

	trace_array_printk(tr, _THIS_IP_,
			   "smart_io_complete: "
			   "ts_insert=%llu ts_issue=%llu ts_complete=%llu "
			   "uid=%u scene_tag=%u is_front=%u thread_role=%u "
			   "io_op=%u io_sz_kb=%u sector=%llu ioprio=%u file_ext=%u file_ext_s=%s "
			   "fs=%s dev_name=%s remap_from=%s remap_mixed=%u dev_type=%u sync=%u inode=%llu "
			   "io_inflight=%u queue_sat=%u cg_io_weight=%u gc=%u cp=%u "
			   "hot_file=%u mem_avail_mb=%u dirty_mb=%u wb_mb=%u swap_mb=%u "
			   "cpu_loadavg_1m=%u online_cpus=%u runqueue=%u dev_util_pct=%u "
			   "io_bw_kbps=%u lat_us=%llu sched_us=%llu dev_us=%llu "
			   "sched_pct=%u p50_r_us=%u p50_w_us=%u p99_r_us=%u p99_w_us=%u "
			   "avg_r_us=%u avg_w_us=%u p50_p99_r=%u p50_p99_w=%u "
			   "reclaim_pages=%llu sys_psi_io=%u blk_bw_util=%u "
			   "render_avg_lat=%u jank_cnt=%u ttff=%u ttir=%u "
			   "cache_hit=%u file_access_pages=%llu file_miss_pages=%llu file_cache_add_pages=%llu file_cache_delete_pages=%llu file_refault_pages=%llu file_fault_count=%llu file_fault_wait_us=%llu direct=%u\n",
			   evt->ts_insert, evt->ts_issue, evt->ts_complete,
			   evt->uid, evt->scene_tag, evt->is_front, evt->thread_role,
			   evt->io_op, evt->io_size_kb, evt->sector, evt->ioprio_class,
			   evt->file_ext, evt->file_ext_str, evt->fs_type,
			   evt->dev_name, evt->remap_from, evt->remap_mixed,
			   evt->device_type, evt->is_sync,
			   evt->inode_hash, evt->io_in_flight, evt->queue_saturate,
			   evt->cgroup_io_weight, evt->f2fs_gc_active,
			   evt->f2fs_cp_active, evt->is_hot_file, evt->mem_avail_mb,
			   evt->dirty_pages_mb, evt->wb_pages_mb, evt->swap_mb,
			   evt->cpu_loadavg_1m, evt->online_cpus, evt->runqueue_len,
			   evt->device_util_pct, evt->io_bw_kbps, evt->latency_us,
			   evt->sched_us, evt->dev_us, evt->sched_pct,
			   evt->p50_read_lat_us, evt->p50_write_lat_us,
			   evt->p99_read_lat_us, evt->p99_write_lat_us,
			   evt->avg_read_lat_us, evt->avg_write_lat_us,
			   evt->p50_p99_ratio_r, evt->p50_p99_ratio_w,
			   evt->reclaim_pages, evt->sys_psi_io, evt->blk_bw_util,
			   evt->render_avg_lat, evt->jank_cnt,
			   evt->time_to_first_frame, evt->time_to_input_response,
			   evt->cache_hit, evt->file_access_pages, evt->file_miss_pages,
			   evt->file_cache_add_pages, evt->file_cache_delete_pages,
			   evt->file_refault_pages, evt->file_fault_count,
			   evt->file_fault_wait_us, evt->direct_path);
}

void smart_io_raw_emit(const char *fmt, ...)
{
	struct trace_array *tr = READ_ONCE(smart_io_trace_array);
	struct va_format vaf;
	va_list args;

	if (unlikely(atomic_read(&rawdata_trace_enabled) == 0 || !tr || !fmt))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	trace_array_printk(tr, _THIS_IP_, "%pV", &vaf);
	va_end(args);
}

void smart_io_trace_debug(const char *fmt, ...)
{
	struct trace_array *tr = READ_ONCE(smart_io_trace_array);
	struct va_format vaf;
	va_list args;

	if (atomic_read(&debug_enabled) == 0 || !fmt) {
		return;
	}

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	smart_io_log_warn("%pV", &vaf);
	va_end(args);

	if (unlikely(!tr))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	trace_array_printk(tr, _THIS_IP_, "%pV", &vaf);
	va_end(args);
}

void smart_io_trace_log(const char *fmt, ...)
{
	struct trace_array *tr = READ_ONCE(smart_io_trace_array);
	struct va_format vaf;
	va_list args;

	if (unlikely(!tr)) {
		tr = instance_retry();
	}

	if (unlikely(!tr || !fmt))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	trace_array_printk(tr, _THIS_IP_, "%pV", &vaf);
	va_end(args);
}
