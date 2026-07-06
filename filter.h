#ifndef FILTER_H
#define FILTER_H

#include <linux/types.h>

int smart_io_policy_set(u32 rate, int uid, bool sync_only, u8 mask);

#endif