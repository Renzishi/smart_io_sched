#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "filter.h"
#include "io_semantics.h"
#include "procfs_export.h"
#include "smart_io_log.h"
#include "smart_io_policy.h"
#include "smart_io_throttle.h"
#include "smart_io_types.h"
#include "system_context.h"
#include "tp_pagecache_demo.h"
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

static ssize_t ux_hp_enable_write(struct file *f, const char __user *ub,
					  size_t c, loff_t *p)
{
	int enabled;
	int ret;

	ret = proc_parse_int(ub, c, &enabled);
	if (ret)
		return ret;
	if (enabled != 0 && enabled != 1)
		return -EINVAL;

	ret = smart_io_throttle_set_ux_hp_enable(enabled != 0);
	if (ret)
		return ret;
	return c;
}

static int ux_hp_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_throttle_get_ux_hp_enable() ? 1 : 0);
	return 0;
}

static int ux_hp_enable_open(struct inode *i, struct file *f)
{
	return single_open(f, ux_hp_enable_show, NULL);
}

static ssize_t rt_read_hp_enable_write(struct file *f, const char __user *ub,
					       size_t c, loff_t *p)
{
	int enabled;
	int ret;

	ret = proc_parse_int(ub, c, &enabled);
	if (ret)
		return ret;
	if (enabled != 0 && enabled != 1)
		return -EINVAL;

	ret = smart_io_throttle_set_rt_read_hp_enable(enabled != 0);
	if (ret)
		return ret;
	return c;
}

static int rt_read_hp_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_throttle_get_rt_read_hp_enable() ? 1 : 0);
	return 0;
}

static int rt_read_hp_enable_open(struct inode *i, struct file *f)
{
	return single_open(f, rt_read_hp_enable_show, NULL);
}

static ssize_t throttle_enable_write(struct file *f, const char __user *ub,
				     size_t c, loff_t *p)
{
	int enabled;
	int ret;

	ret = proc_parse_int(ub, c, &enabled);
	if (ret)
		return ret;
	if (enabled != 0 && enabled != 1)
		return -EINVAL;

	ret = smart_io_throttle_set_enabled(enabled != 0);
	if (ret) {
		smart_io_log_warn("throttle enable rejected: enabled=%d ret=%d\n",
				  enabled, ret);
		return ret;
	}

	return c;
}

static int throttle_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_throttle_get_enabled());
	return 0;
}

static int throttle_enable_open(struct inode *i, struct file *f)
{
	return single_open(f, throttle_enable_show, NULL);
}

static ssize_t queue_rq_throttle_demo_write(struct file *f,
					     const char __user *ub, size_t c,
					     loff_t *p)
{
	int enabled;
	int ret;

	ret = proc_parse_int(ub, c, &enabled);
	if (ret)
		return ret;
	if (enabled != 0 && enabled != 1)
		return -EINVAL;

	ret = smart_io_throttle_set_queue_rq_demo(enabled != 0);
	if (ret)
		return ret;

	return c;
}

static int queue_rq_throttle_demo_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_throttle_get_queue_rq_demo());
	return 0;
}

static int queue_rq_throttle_demo_open(struct inode *i, struct file *f)
{
	return single_open(f, queue_rq_throttle_demo_show, NULL);
}

static ssize_t throttle_ratios_write(struct file *f, const char __user *ub,
				     size_t c, loff_t *p)
{
	char buf[32];
	u32 light_pct;
	u32 medium_pct;
	u32 heavy_pct;
	int ret;

	if (c > sizeof(buf) - 1)
		return -E2BIG;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;
	if (sscanf(strstrip(buf), "%u %u %u", &light_pct, &medium_pct,
		   &heavy_pct) != 3)
		return -EINVAL;

	ret = smart_io_throttle_set_ratios(light_pct, medium_pct, heavy_pct);
	return ret ? ret : c;
}

static int throttle_ratios_show(struct seq_file *m, void *v)
{
	u32 light_pct;
	u32 medium_pct;
	u32 heavy_pct;

	smart_io_throttle_get_ratios(&light_pct, &medium_pct, &heavy_pct);
	seq_printf(m, "%u %u %u\n", light_pct, medium_pct, heavy_pct);
	return 0;
}

static int throttle_ratios_open(struct inode *i, struct file *f)
{
	return single_open(f, throttle_ratios_show, NULL);
}

static const struct proc_ops throttle_ratios_ops = {
	.proc_open = throttle_ratios_open,
	.proc_read = seq_read,
	.proc_write = throttle_ratios_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t action_source_write(struct file *f, const char __user *ub,
				   size_t c, loff_t *p)
{
	char buf[16];
	enum smart_io_action_source source;
	int ret;

	if (c > sizeof(buf) - 1)
		return -E2BIG;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;
	if (!strcmp(strstrip(buf), "fixed"))
		source = SMART_IO_ACTION_SOURCE_FIXED;
	else if (!strcmp(strstrip(buf), "model"))
		source = SMART_IO_ACTION_SOURCE_MODEL;
	else
		return -EINVAL;

	ret = smart_io_throttle_set_action_source(source);
	return ret ? ret : c;
}

static int action_source_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", smart_io_action_source_name(
			   smart_io_throttle_get_action_source()));
	return 0;
}

static int action_source_open(struct inode *i, struct file *f)
{
	return single_open(f, action_source_show, NULL);
}

static const struct proc_ops action_source_ops = {
	.proc_open = action_source_open,
	.proc_read = seq_read,
	.proc_write = action_source_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int fixed_action_parse_level(const char *name,
				    enum smart_io_throttle_level *level)
{
	if (!strcmp(name, "NO"))
		*level = SMART_IO_THROTTLE_NO;
	else if (!strcmp(name, "LIGHT"))
		*level = SMART_IO_THROTTLE_LIGHT;
	else if (!strcmp(name, "MEDIUM"))
		*level = SMART_IO_THROTTLE_MEDIUM;
	else if (!strcmp(name, "HEAVY"))
		*level = SMART_IO_THROTTLE_HEAVY;
	else
		return -EINVAL;
	return 0;
}

static int fixed_action_parse_policy(const char *name,
				     enum smart_io_dispatch_policy *policy)
{
	if (!strcmp(name, "BASELINE"))
		*policy = SMART_IO_DISPATCH_BASELINE;
	else if (!strcmp(name, "SEQ"))
		*policy = SMART_IO_DISPATCH_SEQ;
	else if (!strcmp(name, "SMALL"))
		*policy = SMART_IO_DISPATCH_SMALL;
	else
		return -EINVAL;
	return 0;
}

static ssize_t fixed_action_write(struct file *f, const char __user *ub,
				  size_t c, loff_t *p)
{
	char buf[32];
	char level_name[8];
	char policy_name[12];
	enum smart_io_throttle_level level;
	enum smart_io_dispatch_policy policy;
	int ret;

	if (c > sizeof(buf) - 1)
		return -E2BIG;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;
	if (sscanf(strstrip(buf), "%7s %11s", level_name, policy_name) != 2)
		return -EINVAL;
	ret = fixed_action_parse_level(level_name, &level);
	if (ret)
		return ret;
	ret = fixed_action_parse_policy(policy_name, &policy);
	if (ret)
		return ret;

	ret = smart_io_throttle_set_fixed_action(level, policy);
	return ret ? ret : c;
}

static int fixed_action_show(struct seq_file *m, void *v)
{
	enum smart_io_throttle_level level;
	enum smart_io_dispatch_policy policy;

	smart_io_throttle_get_fixed_action(&level, &policy);
	seq_printf(m, "%s %s\n", smart_io_throttle_level_name(level),
		   smart_io_dispatch_policy_name(policy));
	return 0;
}

static int fixed_action_open(struct inode *i, struct file *f)
{
	return single_open(f, fixed_action_show, NULL);
}

static const struct proc_ops fixed_action_ops = {
	.proc_open = fixed_action_open,
	.proc_read = seq_read,
	.proc_write = fixed_action_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t dev_lat_write(struct file *f, const char __user *ub, size_t c,
			     loff_t *p)
{
	u32 threshold_us;
	int ret;

	ret = proc_parse_u32(ub, c, &threshold_us);
	if (ret)
		return ret;

	ret = smart_io_throttle_set_dev_lat_threshold(threshold_us);
	if (ret)
		return ret;

	return c;
}

static int dev_lat_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", smart_io_throttle_get_dev_lat_threshold());
	return 0;
}

static int dev_lat_open(struct inode *i, struct file *f)
{
	return single_open(f, dev_lat_show, NULL);
}

static const struct proc_ops dev_lat_ops = {
	.proc_open = dev_lat_open,
	.proc_read = seq_read,
	.proc_write = dev_lat_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t background_deadline_write(struct file *f,
					 const char __user *ub, size_t c,
					 loff_t *p)
{
	u32 deadline_ms;
	int ret;

	ret = proc_parse_u32(ub, c, &deadline_ms);
	if (ret)
		return ret;
	ret = smart_io_throttle_set_background_deadline_ms(deadline_ms);
	return ret ? ret : c;
}

static int background_deadline_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", smart_io_throttle_get_background_deadline_ms());
	return 0;
}

static int background_deadline_open(struct inode *i, struct file *f)
{
	return single_open(f, background_deadline_show, NULL);
}

static const struct proc_ops background_deadline_ops = {
	.proc_open = background_deadline_open,
	.proc_read = seq_read,
	.proc_write = background_deadline_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

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
	struct smart_io_throttle_stats throttle_stats;
	u32 ins;
	u32 iss;
	u32 cmp;
	u32 wait;
	u32 flight;
	u32 lost_cmp;

	if (!atomic_read(&smart_io_enabled)) {
		seq_puts(m, "disabled\n");
	} else {
		smart_io_get_io_stats(&ins, &iss, &cmp, &wait, &flight,
				      &lost_cmp);
		seq_printf(m, "insert:%u issue:%u complete:%u waiting:%u in_flight:%u lost_complete:%u\n",
			   ins, iss, cmp, wait, flight, lost_cmp);
	}

	smart_io_throttle_get_stats(&throttle_stats);
	seq_printf(m,
		   "io_throttle enabled:%u active:%u inference_pending:%u action_valid:%u source:%s session_id:%llu control_id:%llu decision_id:%llu level:%s policy:%s ratios:%u/%u/%u resolved_ratio:%u queue_max:%u target_depth:%u current_depth:%u bg_current_depth:%u reserved_depth:%u issued_depth:%u metadata:capacity=%u,inuse=%u pending:fg=%u,bg=%u gate:%s feedback:%s feedback_rq_id:%llu feedback_samples:count=%u,mean_dev_lat_us=%u,max_dev_lat_us=%u threshold_us:%u background_deadline_ms:%u deadline_forced_active:%u\n",
		   throttle_stats.enabled ? 1U : 0U,
		   throttle_stats.active ? 1U : 0U,
		   throttle_stats.inference_pending ? 1U : 0U,
		   throttle_stats.action_valid ? 1U : 0U,
		   smart_io_action_source_name(throttle_stats.action_source),
		   throttle_stats.session_id, throttle_stats.control_id,
		   throttle_stats.decision_id,
		   smart_io_throttle_level_name(throttle_stats.level),
		   smart_io_dispatch_policy_name(throttle_stats.policy),
		   throttle_stats.light_pct, throttle_stats.medium_pct,
		   throttle_stats.heavy_pct, throttle_stats.resolved_ratio_pct,
		   throttle_stats.queue_max, throttle_stats.target_depth,
			 throttle_stats.current_depth, throttle_stats.bg_current_depth,
			 throttle_stats.current_depth,
			 throttle_stats.issued_depth, throttle_stats.meta_capacity,
		   throttle_stats.meta_inuse,
		   throttle_stats.fg_pending, throttle_stats.bg_pending,
		   smart_io_gate_reason_name(throttle_stats.gate_reason),
		   smart_io_feedback_state_name(throttle_stats.feedback_state),
		   throttle_stats.feedback_rq_id,
		   throttle_stats.feedback_vote_count,
			 throttle_stats.feedback_mean_dev_lat_us,
			 throttle_stats.feedback_max_dev_lat_us,
			 throttle_stats.dev_lat_threshold_us,
		   throttle_stats.background_deadline_ms,
		   throttle_stats.deadline_forced_active ? 1U : 0U);
	seq_printf(m,
		   "io_throttle_counts queues:%u inference:%llu timeout:%llu invalid:%llu dispatched:fg=%llu,bg=%llu bg_blocked:%llu bg_deadline_forced:%llu selection:baseline=%llu,seq=%llu,seq_fallback=%llu,small=%llu depth_anomalies:%llu feedback_rebind:%llu allocation_failures:%llu\n",
		   throttle_stats.active_queues, throttle_stats.inference_count,
		   throttle_stats.timeout_count, throttle_stats.invalid_result_count,
		   throttle_stats.fg_dispatched,
		   throttle_stats.bg_dispatched, throttle_stats.bg_blocked,
		   throttle_stats.bg_deadline_forced,
		   throttle_stats.baseline_hits, throttle_stats.seq_hits,
		   throttle_stats.seq_fallbacks, throttle_stats.small_hits,
		   throttle_stats.depth_anomalies,
		   throttle_stats.feedback_rebind_count,
		   throttle_stats.allocation_failures);
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

static ssize_t pagecache_demo_write(struct file *f, const char __user *ub,
					    size_t c, loff_t *p)
{
	char buf[8];
	int val;

	if (c > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ub, c))
		return -EFAULT;
	buf[c] = 0;
	if (kstrtoint(strstrip(buf), 10, &val))
		return -EINVAL;

	smart_io_pagecache_demo_set_enabled(val != 0);
	smart_io_log_info("pagecache demo %s.\n", val ? "enabled" : "disabled");
	return c;
}

static int pagecache_demo_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", smart_io_pagecache_demo_enabled() ? 1 : 0);
	return 0;
}

static int pagecache_demo_open(struct inode *i, struct file *f)
{
	return single_open(f, pagecache_demo_show, NULL);
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
		.name = "throttle_enable",
		.ops = &(struct proc_ops){
			.proc_open = throttle_enable_open,
			.proc_read = seq_read,
			.proc_write = throttle_enable_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "ux_hp_enable",
		.ops = &(struct proc_ops){
			.proc_open = ux_hp_enable_open,
			.proc_read = seq_read,
			.proc_write = ux_hp_enable_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "rt_read_hp_enable",
		.ops = &(struct proc_ops){
			.proc_open = rt_read_hp_enable_open,
			.proc_read = seq_read,
			.proc_write = rt_read_hp_enable_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "queue_rq_throttle_demo",
		.ops = &(struct proc_ops){
			.proc_open = queue_rq_throttle_demo_open,
			.proc_read = seq_read,
			.proc_write = queue_rq_throttle_demo_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "throttle_ratios",
		.ops = &throttle_ratios_ops,
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "action_source",
		.ops = &action_source_ops,
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "fixed_action",
		.ops = &fixed_action_ops,
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "dev_lat",
		.ops = &dev_lat_ops,
		.mode = 0644,
		.exist = false,
	},
	{
		.name = "background_deadline_ms",
		.ops = &background_deadline_ops,
		.mode = 0644,
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
	{
		.name = "pagecache_demo",
		.ops = &(struct proc_ops){
			.proc_open = pagecache_demo_open,
			.proc_read = seq_read,
			.proc_write = pagecache_demo_write,
			.proc_lseek = seq_lseek,
			.proc_release = single_release,
		},
		.mode = 0666,
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
