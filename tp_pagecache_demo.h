#ifndef TP_PAGECACHE_DEMO_H
#define TP_PAGECACHE_DEMO_H

#include <linux/types.h>

int register_pagecache_demo(void);
void unregister_pagecache_demo(void);
void smart_io_pagecache_demo_set_enabled(bool enabled);
bool smart_io_pagecache_demo_enabled(void);
void smart_io_pagecache_demo_set_observe(bool enabled);
bool smart_io_pagecache_demo_observe_enabled(void);
void smart_io_pagecache_demo_note_read(u32 uid, u64 inode_hash,
					       u64 folio_index, u16 file_ext,
					       bool is_front);

#endif
