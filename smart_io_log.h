#ifndef SMART_IO_LOG_H
#define SMART_IO_LOG_H

#include <linux/printk.h>

#define SMART_IO_LOG_NAME "SMART_IO_SCHED"

#define smart_io_log_err(fmt, ...) \
	pr_err(SMART_IO_LOG_NAME ": " fmt, ##__VA_ARGS__)
#define smart_io_log_warn(fmt, ...) \
	pr_warn(SMART_IO_LOG_NAME ": " fmt, ##__VA_ARGS__)
#define smart_io_log_info(fmt, ...) \
	pr_info(SMART_IO_LOG_NAME ": " fmt, ##__VA_ARGS__)
#define smart_io_log_dbg(fmt, ...) \
	pr_debug(SMART_IO_LOG_NAME ": " fmt, ##__VA_ARGS__)

#endif
