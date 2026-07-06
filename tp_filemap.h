#ifndef TP_FILEMAP_H
#define TP_FILEMAP_H

int register_filemap_tracepoints(void);
void unregister_filemap_tracepoints(void);
void smart_io_filemap_agg_flush(void);

#endif
