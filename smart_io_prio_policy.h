/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMART_IO_PRIO_H
#define SMART_IO_PRIO_H

#include <block/blk.h>

#include "smart_io_types.h"

/*
 * blk-mq copies bio->bi_ioprio into the request only after .limit_depth() has
 * run, so the elevator's tag-admission hook cannot see ioprio class or our hint
 * bits. Stamp the foreground exemption on the bio instead, using the blk_opf bit
 * the kernel reserves for the submitter (REQ_FS_PRIVATE, bit 26; btrfs is its
 * only consumer today). Set by smart_io_mark_bio_hint(), read by dd_limit_depth().
 */
#define SMART_IO_OPF_SCHED_EXEMPT	REQ_FS_PRIVATE

bool is_hp_ioprio(u16 ioprio);
bool is_vip_ioprio(u16 ioprio);
bool has_smart_ioprio_hint(u16 ioprio);
bool is_vip_request(const struct request *rq);
void mark_request_vip_pi(struct request *rq);
void smart_io_mark_bio_hint(struct bio *bio);
void smart_io_mark_bio_write(struct bio *bio);
void smart_io_mark_bio_vip_pi(struct bio *bio);

#endif