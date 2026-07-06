#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/psi.h>
#include <linux/sched/clock.h>
#include <linux/sched/loadavg.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include "io_semantics.h"
#include "smart_io_log.h"
#include "tp_psi.h"

#if defined(CONFIG_PSI) && defined(CONFIG_TRACEPOINTS) && defined(CONFIG_ANDROID_VENDOR_HOOKS)

#define SMART_IO_PSI_FREQ (2 * HZ + 1)
#define SMART_IO_EXP_10S 1677

static void tp_psi_group_cb(void *ignore, struct psi_group *group);

struct psi_tracepoint_entry {
	const char *name;
	void *func;
	struct tracepoint *tp;
	bool init;
};

static struct psi_tracepoint_entry psi_tps[] = {
	{ .name = "android_vh_psi_group", .func = tp_psi_group_cb },
};

static unsigned long fixed_power_int(unsigned long x,
					     unsigned int frac_bits,
					     unsigned int n)
{
	unsigned long result = 1UL << frac_bits;

	if (!n)
		return result;

	for (;;) {
		if (n & 1) {
			result *= x;
			result += 1UL << (frac_bits - 1);
			result >>= frac_bits;
		}
		n >>= 1;
		if (!n)
			break;
		x *= x;
		x += 1UL << (frac_bits - 1);
		x >>= frac_bits;
	}

	return result;
}

static unsigned long smart_io_calc_load_n(unsigned long load,
					  unsigned long exp,
					  unsigned long active,
					  unsigned int n)
{
	return calc_load(load, fixed_power_int(exp, FSHIFT, n), active);
}

static u32 avg_fixed_to_x100(unsigned long avg)
{
	return LOAD_INT(avg) * 100 + LOAD_FRAC(avg);
}

static unsigned long calc_io_some_avg10(struct psi_group *group,
						 u64 now)
{
	u64 psi_period = jiffies_to_nsecs(SMART_IO_PSI_FREQ);
	unsigned long avg = group->avg[PSI_IO_SOME][0];
	unsigned long missed_periods = 0;
	u64 expires = group->avg_next_update;
	u64 period;
	u64 sample;
	unsigned long pct;

	if (now < expires)
		return avg;

	if (now - expires >= psi_period)
		missed_periods = div64_u64(now - expires, psi_period);

	period = now - (group->avg_last_update + (missed_periods * psi_period));
	if (!period)
		return avg;

	sample = group->total[PSI_AVGS][PSI_IO_SOME] -
		 group->avg_total[PSI_IO_SOME];
	if (sample > period)
		sample = period;

	if (missed_periods)
		avg = smart_io_calc_load_n(avg, SMART_IO_EXP_10S, 0,
					  missed_periods);

	pct = (unsigned long)div64_u64(sample * 100, period);
	pct *= FIXED_1;
	return calc_load(avg, SMART_IO_EXP_10S, pct);
}

static void tp_psi_group_cb(void *ignore, struct psi_group *group)
{
	u64 now;
	unsigned long avg10;

	(void)ignore;

	if (!group)
		return;
	if (group->parent)
		return;
	if (!mutex_is_locked(&group->avgs_lock))
		return;

	now = sched_clock();
	avg10 = calc_io_some_avg10(group, now);
	set_psi_io_x100(avg_fixed_to_x100(avg10));
}

#define FOR_EACH_PSI_TP(i) \
	for (i = 0; i < ARRAY_SIZE(psi_tps); i++)

static void lookup_psi_tracepoints(struct tracepoint *tp, void *ignore)
{
	int i;

	(void)ignore;

	FOR_EACH_PSI_TP(i) {
		if (!strcmp(psi_tps[i].name, tp->name))
			psi_tps[i].tp = tp;
	}
}

void unregister_psi_tracepoints(void)
{
	int i;

	FOR_EACH_PSI_TP(i) {
		if (psi_tps[i].init) {
			tracepoint_probe_unregister(psi_tps[i].tp, psi_tps[i].func,
						  NULL);
			psi_tps[i].init = false;
		}
	}

	tracepoint_synchronize_unregister();
	smart_io_log_info("psi tracepoints unregistered.\n");
}

int register_psi_tracepoints(void)
{
	int i;
	int ret;

	for_each_kernel_tracepoint(lookup_psi_tracepoints, NULL);

	FOR_EACH_PSI_TP(i) {
		if (!psi_tps[i].tp) {
			smart_io_log_err("tracepoint %s not found\n", psi_tps[i].name);
			unregister_psi_tracepoints();
			return -EINVAL;
		}

		ret = tracepoint_probe_register(psi_tps[i].tp, psi_tps[i].func,
					       NULL);
		if (ret) {
			smart_io_log_err("failed to register %s: %d\n",
					 psi_tps[i].name, ret);
			unregister_psi_tracepoints();
			return ret;
		}
		psi_tps[i].init = true;
		smart_io_log_info("tracepoint %s registered.\n", psi_tps[i].name);
	}

	return 0;
}

#else

int register_psi_tracepoints(void)
{
	smart_io_log_info("psi tracepoints unavailable on this kernel config.\n");
	return 0;
}

void unregister_psi_tracepoints(void)
{
}

#endif