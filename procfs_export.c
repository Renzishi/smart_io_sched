#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "filter.h"
#include "io_semantics.h"
#include "procfs_export.h"
#include "smart_io_log.h"
#include "smart_io_policy.h"
#include "smart_io_types.h"
#include "system_context.h"
#include "trace_instance.h"

#define PROC_DIR "smart_io_sched"

static struct proc_dir_entry *smart_io_dir;

static int smart_io_create_proc_entry(const char *name, umode_t mode,
				      const struct proc_ops *ops)
{
	if (!proc_create(name, mode, smart_io_dir, ops)) {
		smart_io_log_err("failed to create /proc/%s/%s\n", PROC_DIR, name);
		return -ENOMEM;
	}

	return 0;
}

static ssize_t enable_write(struct file *f, const char __user *ub, size_t c,
			    loff_t *p)
{
	char buf[8];
	int val;

	if (c > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;

	if (kstrtoint(strstrip(buf), 10, &val) == 0) {
		atomic_set(&smart_io_enabled, val ? 1 : 0);
		atomic_set(&current_policy.enabled, val ? 1 : 0);
		if (!val)
			smart_io_clear_remap_state();
		smart_io_log_info("capture %s.\n", val ? "enabled" : "disabled");
		if (val) {
			smart_io_trace_log("========== smart_io is enabled! ==========\n");
		} else {
			smart_io_trace_log("========== smart_io is disabled! ==========\n");
		}
	} else {
		smart_io_log_warn("invalid enable value: %s\n", strstrip(buf));
	}
	return c;
}

static int enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&smart_io_enabled));
	return 0;
}

static int enable_open(struct inode *i, struct file *f)
{
	return single_open(f, enable_show, NULL);
}

static ssize_t policy_write(struct file *f, const char __user *ub, size_t c,
			    loff_t *p)
{
	char buf[64];
	u32 rate;
	int uid;
	int sync_only;
	int mask;

	if (c > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;

	if (sscanf(strstrip(buf), "%u %d %d %d", &rate, &uid, &sync_only,
		   &mask) == 4) {
		int ret = smart_io_policy_set(rate, uid, sync_only != 0, (u8)mask);

		if (ret)
			smart_io_log_warn("policy update rejected: rate=%u uid=%d sync_only=%d mask=%d ret=%d\n",
					  rate, uid, sync_only, mask, ret);
		else
			smart_io_log_info("policy updated: rate=%u uid=%d sync_only=%d mask=%d\n",
					  rate, uid, sync_only, mask);
	} else {
		smart_io_log_warn("invalid policy payload: %s\n", strstrip(buf));
	}
	return c;
}

static int proc_parse_int(const char __user *ub, size_t c, int *val)
{
	char buf[32];

	if (c > sizeof(buf) - 1)
		return -E2BIG;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;

	return kstrtoint(strstrip(buf), 10, val);
}

static int proc_parse_u32(const char __user *ub, size_t c, u32 *val)
{
	char buf[32];

	if (c > sizeof(buf) - 1)
		return -E2BIG;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;

	return kstrtou32(strstrip(buf), 10, val);
}

static ssize_t fg_uid_write(struct file *f, const char __user *ub, size_t c,
			    loff_t *p)
{
	int uid;
	int ret;

	ret = proc_parse_int(ub, c, &uid);
	if (ret) {
		smart_io_log_warn("invalid fg_uid payload, ret=%d\n", ret);
		return ret;
	}

	smart_io_set_fg_uid((uid_t)uid);
	smart_io_log_dbg("foreground uid updated: uid=%d\n", uid);
	return c;
}

static int fg_uid_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_get_fg_uid());
	return 0;
}

static int fg_uid_open(struct inode *i, struct file *f)
{
	return single_open(f, fg_uid_show, NULL);
}

static ssize_t fg_main_pid_write(struct file *f, const char __user *ub, size_t c,
				 loff_t *p)
{
	int pid;
	int ret;

	ret = proc_parse_int(ub, c, &pid);
	if (ret) {
		smart_io_log_warn("invalid fg_main_pid payload, ret=%d\n", ret);
		return ret;
	}

	smart_io_set_fg_main_pid((pid_t)pid);
	smart_io_log_dbg("foreground main pid updated: pid=%d\n", pid);
	return c;
}

static int fg_main_pid_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_get_fg_main_pid());
	return 0;
}

static int fg_main_pid_open(struct inode *i, struct file *f)
{
	return single_open(f, fg_main_pid_show, NULL);
}

static ssize_t scene_tag_write(struct file *f, const char __user *ub, size_t c,
			       loff_t *p)
{
	u32 scene_tag;
	int ret;

	ret = proc_parse_u32(ub, c, &scene_tag);
	if (ret) {
		smart_io_log_warn("invalid scene_tag payload, ret=%d\n", ret);
		return ret;
	}

	smart_io_set_scene_tag(scene_tag);
	smart_io_log_dbg("foreground scene updated: scene=%u\n", scene_tag);
	return c;
}

static ssize_t loadavg_1m_x100_write(struct file *f, const char __user *ub,
				     size_t c, loff_t *p)
{
	u32 loadavg_1m_x100;
	int ret;

	ret = proc_parse_u32(ub, c, &loadavg_1m_x100);
	if (ret) {
		smart_io_log_warn("invalid loadavg_1m_x100 payload, ret=%d\n", ret);
		return ret;
	}

	set_loadavg_1m_x100(loadavg_1m_x100);
	return c;
}

static ssize_t render_avg_lat_write(struct file *f, const char __user *ub,
				    size_t c, loff_t *p)
{
	u32 render_avg_lat;
	int ret;

	ret = proc_parse_u32(ub, c, &render_avg_lat);
	if (ret) {
		smart_io_log_warn("invalid render_avg_lat payload, ret=%d\n", ret);
		return ret;
	}

	set_render_avg_lat(render_avg_lat);
	return c;
}

static int render_avg_lat_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", get_render_avg_lat());
	return 0;
}

static int render_avg_lat_open(struct inode *i, struct file *f)
{
	return single_open(f, render_avg_lat_show, NULL);
}

static ssize_t jank_write(struct file *f, const char __user *ub, size_t c,
			  loff_t *p)
{
	u32 jank_cnt;
	int ret;

	ret = proc_parse_u32(ub, c, &jank_cnt);
	if (ret) {
		smart_io_log_warn("invalid jank payload, ret=%d\n", ret);
		return ret;
	}

	set_jank_cnt(jank_cnt);
	return c;
}

static int jank_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", get_jank_cnt());
	return 0;
}

static int jank_open(struct inode *i, struct file *f)
{
	return single_open(f, jank_show, NULL);
}

static int stats_show(struct seq_file *m, void *v)
{
	u32 ins;
	u32 iss;
	u32 cmp;
	u32 wait;
	u32 flight;
	u32 lost_cmp;

	if (!atomic_read(&smart_io_enabled)) {
		seq_puts(m, "disabled\n");
		return 0;
	}

	smart_io_get_io_stats(&ins, &iss, &cmp, &wait, &flight, &lost_cmp);
	seq_printf(m, "insert:%u issue:%u complete:%u waiting:%u in_flight:%u lost_complete:%u\n",
		   ins, iss, cmp, wait, flight, lost_cmp);
	return 0;
}

static int stats_open(struct inode *i, struct file *f)
{
	return single_open(f, stats_show, NULL);
}

static const struct proc_ops stats_ops = {
	.proc_open = stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int event_show(struct seq_file *m, void *v)
{
	struct smart_io_event e;

	smart_io_read_latest_event(&e);
	seq_printf(m,
		   "ts_insert:%llu ts_issue:%llu ts_complete:%llu "
		   "uid:%u scene_tag:%u is_front:%u thread_role:%u "
		   "io_op:%u io_sz_kb:%u sector:%llu ioprio:%u file_ext:%u file_ext_s:%s fs:%s dev_name:%s remap_from:%s remap_mixed:%u dev_type:%u "
		   "io_inflight:%u queue_sat:%u cg_io_weight:%u "
		   "gc:%u cp:%u hot_file:%u "
		   "mem_avail_mb:%u dirty_mb:%u wb_mb:%u swap_mb:%u cpu_loadavg_1m:%u "
		   "online_cpus:%u runqueue:%u dev_util_pct:%u "
		   "io_bw_kbps:%u lat_us:%llu sched_us:%llu dev_us:%llu sched_pct:%u "
		   "p50_r_us:%u p50_w_us:%u p99_r_us:%u p99_w_us:%u avg_r_us:%u avg_w_us:%u "
		   "p50/p99_r:%u p50/p99_w:%u "
		   "reclaim_pages:%llu sys_psi_io:%u blk_bw_util:%u "
		   "render_avg_lat:%u jank_cnt:%u time_to_first_frame:%u "
		   "time_to_input_response:%u cache_hit:%u file_access_pages:%llu file_miss_pages:%llu file_cache_add_pages:%llu file_cache_delete_pages:%llu file_refault_pages:%llu file_fault_count:%llu file_fault_wait_us:%llu\n",
		   e.ts_insert, e.ts_issue, e.ts_complete,
		   e.uid, e.scene_tag, e.is_front, e.thread_role,
		   e.io_op, e.io_size_kb, e.sector, e.ioprio_class, e.file_ext,
		   e.file_ext_str, e.fs_type, e.dev_name, e.remap_from,
		   e.remap_mixed, e.device_type,
		   e.io_in_flight, e.queue_saturate, e.cgroup_io_weight,
		   e.f2fs_gc_active, e.f2fs_cp_active, e.is_hot_file,
		   e.mem_avail_mb, e.dirty_pages_mb, e.wb_pages_mb,
		   e.swap_mb, e.cpu_loadavg_1m, e.online_cpus, e.runqueue_len,
		   e.device_util_pct, e.io_bw_kbps,
		   e.latency_us, e.sched_us, e.dev_us, e.sched_pct,
		   e.p50_read_lat_us, e.p50_write_lat_us, e.p99_read_lat_us,
		   e.p99_write_lat_us, e.avg_read_lat_us, e.avg_write_lat_us,
		   e.p50_p99_ratio_r, e.p50_p99_ratio_w, e.reclaim_pages, e.sys_psi_io,
		   e.blk_bw_util, e.render_avg_lat, e.jank_cnt, e.time_to_first_frame,
		   e.time_to_input_response, e.cache_hit, e.file_access_pages,
		   e.file_miss_pages, e.file_cache_add_pages,
		   e.file_cache_delete_pages, e.file_refault_pages,
		   e.file_fault_count, e.file_fault_wait_us);
	return 0;
}

static int event_open(struct inode *i, struct file *f)
{
	return single_open(f, event_show, NULL);
}

static const struct proc_ops event_ops = {
	.proc_open = event_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t window_sz_ms_write(struct file *f, const char __user *ub,
				  size_t c, loff_t *p)
{
	u32 window_sz_ms;
	int ret;

	ret = proc_parse_u32(ub, c, &window_sz_ms);
	if (ret) {
		smart_io_log_warn("invalid window_sz_ms payload, ret=%d\n", ret);
		return ret;
	}
	smart_io_log_info("window size updated to: %u ms\n", window_sz_ms);

	set_window_sz_ms(window_sz_ms);
	return c;
}

static int window_sz_ms_show(struct seq_file *m, void *v)
{
	u32 window_sz_ms = get_window_sz_ms();
	seq_printf(m, "%u\n", window_sz_ms);
	return 0;
}

static int window_sz_ms_open(struct inode *i, struct file *f)
{
	return single_open(f, window_sz_ms_show, NULL);
}

static ssize_t rawdata_trace_write(struct file *f, const char __user *ub, size_t c,
			  loff_t *p)
{
	char buf[8];
	int val;

	if (c > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;

	if (kstrtoint(strstrip(buf), 10, &val) == 0) {
		smart_io_log_info("raw trace %s.\n", val ? "enabled" : "disabled");
		atomic_set(&rawdata_trace_enabled, val ? 1 : 0);
		if (val) {
			smart_io_trace_log("========== raw trace enabled! ==========\n");
		} else {
			smart_io_trace_log("========== raw trace disabled! ==========\n");
		}
	} else {
		smart_io_log_warn("invalid raw trace value: %s\n", strstrip(buf));
	}
	return c;
}

static int rawdata_trace_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&rawdata_trace_enabled));
	return 0;
}

static int rawdata_trace_open(struct inode *i, struct file *f)
{
	return single_open(f, rawdata_trace_show, NULL);
}

static ssize_t debug_write(struct file *f, const char __user *ub, size_t c,
			  loff_t *p)
{
	char buf[8];
	int val;

	if (c > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;

	if (kstrtoint(strstrip(buf), 10, &val) == 0) {
		smart_io_log_info("debug trace %s.\n", val ? "enabled" : "disabled");
		atomic_set(&debug_enabled, val ? 1 : 0);
		atomic_set(&rawdata_trace_enabled, val ? 1 : 0);
	} else {
		smart_io_log_warn("invalid debug trace value: %s\n", strstrip(buf));
	}
	return c;
}

static int debug_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&debug_enabled));
	return 0;
}

static int debug_open(struct inode *i, struct file *f)
{
	return single_open(f, debug_show, NULL);
}

struct proc_node {
	const char *name;
	const struct proc_ops *ops;
	umode_t mode;
	bool exist;
};

struct proc_node proc_nodes[] = {
	{ 
		.name = "enable",
		.ops = &(struct proc_ops){
			.proc_open = enable_open,
			.proc_read = seq_read,
			.proc_write = enable_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0200,
		.exist = false,
	},
	{
		.name = "policy",
		.ops = &(struct proc_ops){
			.proc_write = policy_write,
		},
		.mode = 0200,
		.exist = false,
	},
	{
		.name = "fg_uid",
		.ops = &(struct proc_ops){
			.proc_open = fg_uid_open,
			.proc_read = seq_read,
			.proc_write = fg_uid_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "fg_main_pid",
		.ops = &(struct proc_ops){
			.proc_open = fg_main_pid_open,
			.proc_read = seq_read,
			.proc_write = fg_main_pid_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "scene_tag",
		.ops = &(struct proc_ops){
			.proc_write = scene_tag_write,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "loadavg_1m_x100",
		.ops = &(struct proc_ops){
			.proc_write = loadavg_1m_x100_write,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "render_avg_lat",
		.ops = &(struct proc_ops){
			.proc_open = render_avg_lat_open,
			.proc_read = seq_read,
			.proc_write = render_avg_lat_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "jank",
		.ops = &(struct proc_ops){
			.proc_open = jank_open,
			.proc_read = seq_read,
			.proc_write = jank_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "stats",
		.ops = &stats_ops,
		.mode = 0400,
		.exist = false,
	},
	{
		.name = "last_event",
		.ops = &event_ops,
		.mode = 0400,
		.exist = false,
	},
	{
		.name = "window_sz_ms",
		.ops = &(struct proc_ops){
			.proc_open = window_sz_ms_open,
			.proc_read = seq_read,
			.proc_write = window_sz_ms_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0666,
		.exist = false,
	},
	{
		.name = "rawdata_trace",
		.ops = &(struct proc_ops){
			.proc_write = rawdata_trace_write,
			.proc_read = seq_read,
			.proc_open = rawdata_trace_open,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0200,
		.exist = false,
	},
	{
		.name = "debug_on",
		.ops = &(struct proc_ops){
			.proc_write = debug_write,
			.proc_read = seq_read,
			.proc_open = debug_open,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0200,
		.exist = false,
	},
};

static void smart_io_remove_proc_entries(void)
{
	if (!smart_io_dir)
		return;

	for (size_t i = 0; i < ARRAY_SIZE(proc_nodes); i++) {
		const struct proc_node *node = &proc_nodes[i];
		if (node->exist) {
			remove_proc_entry(node->name, smart_io_dir);
		}
	}
	remove_proc_entry(PROC_DIR, NULL);
	smart_io_dir = NULL;
}

int smart_io_proc_init(void)
{
	int ret;

	smart_io_dir = proc_mkdir(PROC_DIR, NULL);
	if (!smart_io_dir)
		return -ENOMEM;

	for (size_t i = 0; i < ARRAY_SIZE(proc_nodes); i++) {
		struct proc_node *node = &proc_nodes[i];
		ret = smart_io_create_proc_entry(node->name, node->mode, node->ops);
		if (ret) {
			smart_io_log_err("failed to create /proc/%s/%s\n", PROC_DIR, node->name);
			goto err;
		}
		node->exist = true;
	}

	smart_io_log_info("procfs interface ready at /proc/%s.\n", PROC_DIR);
	return 0;

err:
	smart_io_remove_proc_entries();
	return ret;
}

void smart_io_proc_exit(void)
{
	smart_io_remove_proc_entries();
	smart_io_log_info("procfs interface removed.\n");
}
