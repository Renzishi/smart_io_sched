#ifndef TP_FILEMAP_H
#define TP_FILEMAP_H

#include <linux/types.h>

int register_filemap_tracepoints(void);
void unregister_filemap_tracepoints(void);
int register_filemap_fault_tracepoints(void);
void unregister_filemap_fault_tracepoints(void);
bool smart_io_filemap_fault_active(void);
void smart_io_filemap_agg_flush(void);

#endif
