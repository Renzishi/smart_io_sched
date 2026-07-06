#ifndef BLOCK_HELPER_H
#define BLOCK_HELPER_H

#include <linux/types.h>
#include <linux/bio.h>

#include "smart_io_types.h"

bool extract_vfs_info(struct bio *bio, u64 *inode_hash, u16 *ext, u64 *folio_index,
		      char *ext_str, size_t ext_str_len,
		      char *fs_type, size_t fs_type_len);
void extract_device_name(struct request *rq, char *dev_name, size_t dev_name_len);
u32 get_cgroup_weight(struct request *rq);

#endif