#include <linux/printk.h>

#include "smart_io_log.h"
#include "tp_binder.h"
#include "tp_f2fs.h"
#include "tp_filemap.h"
#include "tp_pagecache_demo.h"
#include "tp_psi.h"
#include "tp_sched.h"

extern int register_block_tracepoints(void);
extern void unregister_block_tracepoints(void);

int smart_io_tp_init(void)
{
	int ret;

	ret = register_block_tracepoints();
	if (ret) {
		smart_io_log_err("failed to register block tracepoints: %d\n", ret);
		return ret;
	}

	ret = register_f2fs_tracepoints();
	if (ret) {
		unregister_block_tracepoints();
		smart_io_log_err("failed to register f2fs tracepoints: %d\n", ret);
		return ret;
	}

	ret = register_psi_tracepoints();
	if (ret) {
		unregister_f2fs_tracepoints();
		unregister_block_tracepoints();
		smart_io_log_err("failed to register psi tracepoints: %d\n", ret);
		return ret;
	}

	ret = register_pagecache_demo();
	if (ret) {
		unregister_psi_tracepoints();
		unregister_f2fs_tracepoints();
		unregister_block_tracepoints();
		return ret;
	}

	// ret = register_binder_tracepoints();
	// if (ret) {
	// 	unregister_filemap_tracepoints();
	// 	unregister_psi_tracepoints();
	// 	unregister_f2fs_tracepoints();
	// 	unregister_block_tracepoints();
	// 	smart_io_log_err("failed to register binder tracepoints: %d\n", ret);
	// 	return ret;
	// }

	// ret = register_sched_tracepoints();
	// if (ret) {
	// 	unregister_binder_tracepoints();
	// 	unregister_filemap_tracepoints();
	// 	unregister_psi_tracepoints();
	// 	unregister_f2fs_tracepoints();
	// 	unregister_block_tracepoints();
	// 	smart_io_log_err("sched tracepoints unavailable: %d\n", ret);
	// 	return ret;
	// }

	smart_io_log_info("tracepoints registered.\n");
	return 0;
}

void smart_io_tp_exit(void)
{
	// unregister_sched_tracepoints();
	// unregister_binder_tracepoints();
	unregister_pagecache_demo();
	unregister_psi_tracepoints();
	unregister_f2fs_tracepoints();
	unregister_block_tracepoints();
	smart_io_log_info("tracepoints unregistered.\n");
}
