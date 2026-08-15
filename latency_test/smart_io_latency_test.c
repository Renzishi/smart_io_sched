// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "smart_io_latency_uapi.h"

#define SMART_IO_LATENCY_TRIGGER_DELAY_US 2000U

struct smart_io_latency_ctx {
	struct miscdevice miscdev;
	struct hrtimer trigger_timer;
	wait_queue_head_t request_waitq;
	spinlock_t lock;
	atomic_t opened;
	bool stopping;
	bool read_armed;
	bool request_ready;
	bool response_pending;
	bool last_valid;
	u64 next_sequence_id;
	u64 current_wake_ts_ns;
	u32 current_trigger_cpu;
	bool current_waiter_present;
	struct smart_io_latency_request request;
	struct smart_io_latency_action published_action;
	struct smart_io_latency_result last_result;
};

static struct smart_io_latency_ctx latency_ctx;

static void smart_io_latency_reset_session_locked(
		struct smart_io_latency_ctx *ctx)
{
	ctx->read_armed = false;
	ctx->request_ready = false;
	ctx->response_pending = false;
	ctx->last_valid = false;
	ctx->next_sequence_id = 0;
	ctx->current_wake_ts_ns = 0;
	ctx->current_trigger_cpu = 0;
	ctx->current_waiter_present = false;
	memset(&ctx->request, 0, sizeof(ctx->request));
	memset(&ctx->published_action, 0, sizeof(ctx->published_action));
	memset(&ctx->last_result, 0, sizeof(ctx->last_result));
}

static enum hrtimer_restart
smart_io_latency_trigger_timer(struct hrtimer *timer)
{
	struct smart_io_latency_ctx *ctx =
		container_of(timer, struct smart_io_latency_ctx, trigger_timer);
	unsigned long flags;
	bool waiter_present;
	bool wake = false;

	waiter_present = waitqueue_active(&ctx->request_waitq);
	spin_lock_irqsave(&ctx->lock, flags);
	if (!ctx->stopping && ctx->read_armed && !ctx->response_pending) {
		ctx->next_sequence_id++;
		if (!ctx->next_sequence_id)
			ctx->next_sequence_id++;

		ctx->request.version = SMART_IO_LATENCY_ABI_VERSION;
		ctx->request.size = sizeof(ctx->request);
		ctx->request.sequence_id = ctx->next_sequence_id;
		ctx->request.trigger_delay_us =
			SMART_IO_LATENCY_TRIGGER_DELAY_US;
		ctx->request.reserved = 0;
		ctx->current_trigger_cpu = raw_smp_processor_id();
		ctx->current_waiter_present = waiter_present;
		ctx->current_wake_ts_ns = ktime_get_ns();
		ctx->response_pending = true;
		ctx->request_ready = true;
		wake = true;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (wake)
		wake_up_interruptible(&ctx->request_waitq);
	return HRTIMER_NORESTART;
}

static int smart_io_latency_open(struct inode *inode, struct file *file)
{
	struct smart_io_latency_ctx *ctx = &latency_ctx;
	unsigned long flags;
	int ret;

	ret = nonseekable_open(inode, file);
	if (ret)
		return ret;
	if (atomic_cmpxchg(&ctx->opened, 0, 1))
		return -EBUSY;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->stopping) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		atomic_set(&ctx->opened, 0);
		return -ESHUTDOWN;
	}
	smart_io_latency_reset_session_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);
	file->private_data = ctx;
	return 0;
}

static int smart_io_latency_release(struct inode *inode, struct file *file)
{
	struct smart_io_latency_ctx *ctx = file->private_data;
	unsigned long flags;

	(void)inode;
	if (!ctx)
		return 0;

	hrtimer_cancel(&ctx->trigger_timer);
	spin_lock_irqsave(&ctx->lock, flags);
	smart_io_latency_reset_session_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);
	wake_up_interruptible(&ctx->request_waitq);
	atomic_set(&ctx->opened, 0);
	return 0;
}

static ssize_t smart_io_latency_read(struct file *file, char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct smart_io_latency_ctx *ctx = file->private_data;
	struct smart_io_latency_request request;
	unsigned long flags;
	int ret;

	(void)ppos;
	if (!ctx)
		return -ENODEV;
	if (count != sizeof(request))
		return -EMSGSIZE;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->stopping) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -ESHUTDOWN;
	}
	if (ctx->read_armed || ctx->response_pending) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EBUSY;
	}
	ctx->read_armed = true;
	ctx->request_ready = false;
	ctx->last_valid = false;
	spin_unlock_irqrestore(&ctx->lock, flags);

	hrtimer_start(&ctx->trigger_timer,
		      ns_to_ktime((u64)SMART_IO_LATENCY_TRIGGER_DELAY_US *
				  NSEC_PER_USEC),
		      HRTIMER_MODE_REL);
	ret = wait_event_interruptible(ctx->request_waitq,
				       READ_ONCE(ctx->request_ready) ||
				       READ_ONCE(ctx->stopping));
	if (ret) {
		hrtimer_cancel(&ctx->trigger_timer);
		spin_lock_irqsave(&ctx->lock, flags);
		if (ctx->read_armed) {
			ctx->request_ready = false;
			ctx->response_pending = false;
			ctx->read_armed = false;
		}
		spin_unlock_irqrestore(&ctx->lock, flags);
		return ret;
	}

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->stopping) {
		ctx->read_armed = false;
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -ESHUTDOWN;
	}
	request = ctx->request;
	ctx->request_ready = false;
	ctx->read_armed = false;
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (copy_to_user(buffer, &request, sizeof(request))) {
		spin_lock_irqsave(&ctx->lock, flags);
		if (ctx->response_pending &&
		    ctx->request.sequence_id == request.sequence_id)
			ctx->response_pending = false;
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EFAULT;
	}
	return sizeof(request);
}

static ssize_t smart_io_latency_write(struct file *file,
				      const char __user *buffer, size_t count,
				      loff_t *ppos)
{
	struct smart_io_latency_ctx *ctx = file->private_data;
	struct smart_io_latency_action action;
	struct smart_io_latency_result result;
	unsigned long flags;
	u64 write_enter_ts_ns;
	u64 decision_end_ts_ns;
	u32 write_cpu;

	(void)ppos;
	if (!ctx)
		return -ENODEV;
	write_enter_ts_ns = ktime_get_ns();
	write_cpu = get_cpu();
	put_cpu();

	if (count != sizeof(action))
		return -EMSGSIZE;
	if (copy_from_user(&action, buffer, sizeof(action)))
		return -EFAULT;
	if (action.version != SMART_IO_LATENCY_ABI_VERSION ||
	    action.size != sizeof(action) ||
	    action.throttle_level != SMART_IO_LATENCY_THROTTLE_MEDIUM ||
	    action.dispatch_policy != SMART_IO_LATENCY_DISPATCH_BASELINE)
		return -EINVAL;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->stopping) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -ESHUTDOWN;
	}
	if (!ctx->response_pending) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EAGAIN;
	}
	if (action.sequence_id != ctx->request.sequence_id) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -ESTALE;
	}

	if (ctx->current_waiter_present)
		ctx->published_action = action;
	decision_end_ts_ns = ktime_get_ns();
	memset(&result, 0, sizeof(result));
	result.version = SMART_IO_LATENCY_ABI_VERSION;
	result.size = sizeof(result);
	result.sequence_id = action.sequence_id;
	result.wake_ts_ns = ctx->current_wake_ts_ns;
	result.write_enter_ts_ns = write_enter_ts_ns;
	result.decision_end_ts_ns = decision_end_ts_ns;
	result.wake_to_write_enter_ns =
		write_enter_ts_ns - ctx->current_wake_ts_ns;
	result.kernel_apply_ns = decision_end_ts_ns - write_enter_ts_ns;
	result.total_ns = decision_end_ts_ns - ctx->current_wake_ts_ns;
	result.trigger_cpu = ctx->current_trigger_cpu;
	result.write_cpu = write_cpu;
	result.waiter_present = ctx->current_waiter_present;

	if (!ctx->current_waiter_present) {
		result.status = SMART_IO_LATENCY_STATUS_NO_WAITER;
	} else if (result.total_ns > SMART_IO_LATENCY_DEADLINE_NS) {
		memset(&ctx->published_action, 0,
		       sizeof(ctx->published_action));
		result.status = SMART_IO_LATENCY_STATUS_LATE;
	} else {
		result.status = SMART_IO_LATENCY_STATUS_APPLIED;
	}
	ctx->last_result = result;
	ctx->last_valid = true;
	ctx->response_pending = false;
	spin_unlock_irqrestore(&ctx->lock, flags);
	return count;
}

static long smart_io_latency_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct smart_io_latency_ctx *ctx = file->private_data;
	struct smart_io_latency_result result;
	unsigned long flags;

	if (!ctx)
		return -ENODEV;
	if (cmd != SMART_IO_LATENCY_IOC_GET_LAST)
		return -ENOTTY;

	spin_lock_irqsave(&ctx->lock, flags);
	if (!ctx->last_valid) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EAGAIN;
	}
	result = ctx->last_result;
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (copy_to_user((void __user *)arg, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static const struct file_operations smart_io_latency_fops = {
	.owner = THIS_MODULE,
	.open = smart_io_latency_open,
	.release = smart_io_latency_release,
	.read = smart_io_latency_read,
	.write = smart_io_latency_write,
	.unlocked_ioctl = smart_io_latency_ioctl,
};

static int __init smart_io_latency_init(void)
{
	struct smart_io_latency_ctx *ctx = &latency_ctx;

	spin_lock_init(&ctx->lock);
	init_waitqueue_head(&ctx->request_waitq);
	atomic_set(&ctx->opened, 0);
	ctx->stopping = false;
	hrtimer_init(&ctx->trigger_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ctx->trigger_timer.function = smart_io_latency_trigger_timer;
	ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctx->miscdev.name = SMART_IO_LATENCY_DEVICE_NAME;
	ctx->miscdev.fops = &smart_io_latency_fops;
	return misc_register(&ctx->miscdev);
}

static void __exit smart_io_latency_exit(void)
{
	struct smart_io_latency_ctx *ctx = &latency_ctx;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	ctx->stopping = true;
	spin_unlock_irqrestore(&ctx->lock, flags);
	misc_deregister(&ctx->miscdev);
	hrtimer_cancel(&ctx->trigger_timer);
	wake_up_interruptible(&ctx->request_waitq);
}

module_init(smart_io_latency_init);
module_exit(smart_io_latency_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Smart I/O daemon fixed-action round-trip latency test");
