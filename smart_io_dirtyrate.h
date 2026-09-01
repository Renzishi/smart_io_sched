/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMART_IO_DIRTYRATE_H
#define SMART_IO_DIRTYRATE_H

#include <linux/types.h>

int smart_io_dirtyrate_init(void);
void smart_io_dirtyrate_fg_uid_changed(void);
int smart_io_dirtyrate_set_enabled(bool enabled);
bool smart_io_dirtyrate_get_enabled(void);
int smart_io_dirtyrate_set_active(bool active);
int smart_io_dirtyrate_set_wkbps(u64 wkbps);
u64 smart_io_dirtyrate_get_wkbps(void);

#endif
