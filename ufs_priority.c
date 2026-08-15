// SPDX-License-Identifier: GPL-2.0
#include <linux/tracepoint.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <trace/hooks/ufshcd.h>
#include <ufs/ufs.h>
#include <ufs/ufshcd.h>

#include "smart_deadline.h"
#include "smart_io_log.h"
#include "smart_io_throttle.h"
#include "ufs_priority.h"

static void smart_io_ufs_send_command(void *unused, struct ufs_hba *hba,
				      struct ufshcd_lrb *lrbp)
{
	struct request *rq;

	if (!smart_io_throttle_enabled() || !lrbp || !lrbp->cmd ||
	    !lrbp->ucd_req_ptr)
		return;

	rq = scsi_cmd_to_rq(lrbp->cmd);
	if (!smart_deadline_rq_is_foreground(rq))
		return;

	lrbp->ucd_req_ptr->header.flags |= UPIU_CMD_FLAGS_CP;
}

static void smart_io_ufs_prepare_command(void *unused, struct ufs_hba *hba,
					 struct request *rq,
					 struct ufshcd_lrb *lrbp, int *err)
{
	if (!hba || !rq || !lrbp || !err || *err)
		return;
	if (smart_io_throttle_ufs_should_requeue(rq))
		*err = SCSI_MLQUEUE_HOST_BUSY;
}

int smart_io_ufs_priority_init(void)
{
	int ret;

	ret = register_trace_android_vh_ufs_prepare_command(
		smart_io_ufs_prepare_command, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_ufs_send_command(
		smart_io_ufs_send_command, NULL);
	if (ret) {
		unregister_trace_android_vh_ufs_prepare_command(
			smart_io_ufs_prepare_command, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}
	smart_io_log_info("UFS command priority hooks registered.\n");

	return 0;
}

void smart_io_ufs_priority_exit(void)
{
	unregister_trace_android_vh_ufs_prepare_command(
		smart_io_ufs_prepare_command, NULL);
	unregister_trace_android_vh_ufs_send_command(
		smart_io_ufs_send_command, NULL);
	tracepoint_synchronize_unregister();
	smart_io_log_info("UFS command priority hooks unregistered.\n");
}
