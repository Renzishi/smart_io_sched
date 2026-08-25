#include <linux/module.h>

#include "io_semantics.h"
#include "procfs_export.h"
#include "smart_deadline.h"
#include "smart_io_log.h"
#include "system_context.h"
#include "trace_instance.h"
#include "ufs_priority.h"

atomic_t smart_io_enabled = ATOMIC_INIT(0);

extern int smart_io_tp_init(void);
extern void smart_io_tp_exit(void);

static int __init smart_io_init(void)
{
	int ret;

	ret = smart_io_semantics_init();
	if (ret) {
		smart_io_log_err("smart_io_semantics_init failed: %d\n", ret);
		return ret;
	}

	ret = smart_io_proc_init();
	if (ret) {
		smart_io_log_err("smart_io_proc_init failed: %d\n", ret);
		goto err_semantics;
	}

	ret = smart_io_trace_instance_init();
	if (ret) {
		smart_io_log_err("smart_io_trace_instance_init failed: %d\n", ret);
	}

	ret = smart_io_tp_init();
	if (ret) {
		smart_io_log_err("smart_io_tp_init failed: %d\n", ret);
		goto err_trace_instance;
	}

	ret = smart_io_ctx_init();
	if (ret) {
		smart_io_log_err("smart_io_ctx_init failed: %d\n", ret);
		goto err_tp;
	}

	ret = smart_deadline_init();
	if (ret) {
		smart_io_log_err("smart_deadline_init failed: %d\n", ret);
		goto err_ctx;
	}

	ret = smart_io_ufs_priority_init();
	if (ret) {
		smart_io_log_err("smart_io_ufs_priority_init failed: %d\n", ret);
		goto err_deadline;
	}

	smart_io_log_info("module loaded. Tracepoint-based, GKI cautious.\n");
	return 0;

err_deadline:
	smart_deadline_exit();
err_ctx:
	smart_io_ctx_exit();
err_tp:
	smart_io_tp_exit();
err_trace_instance:
	smart_io_trace_instance_exit();
	smart_io_proc_exit();
err_semantics:
	smart_io_semantics_exit();
	return ret;
}

static void __exit smart_io_exit(void)
{
	smart_io_ufs_priority_exit();
	smart_deadline_exit();
	smart_io_ctx_exit();
	smart_io_tp_exit();
	smart_io_trace_instance_exit();
	smart_io_proc_exit();
	smart_io_semantics_exit();
	smart_io_log_info("module unloaded.\n");
}

module_init(smart_io_init);
module_exit(smart_io_exit);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(MINIDUMP);
