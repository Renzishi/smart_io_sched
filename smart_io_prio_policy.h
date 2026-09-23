/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMART_IO_PRIO_H
#define SMART_IO_PRIO_H

#include <block/blk.h>

#include "smart_io_types.h"

bool is_hp_ioprio(u16 ioprio);
bool is_vip_ioprio(u16 ioprio);
bool has_smart_ioprio_hint(u16 ioprio);
bool is_vip_request(const struct request *rq);
void smart_io_mark_bio_hint(struct bio *bio);

#endif