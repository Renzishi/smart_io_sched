#ifndef SMART_IO_TRACE_INSTANCE_H
#define SMART_IO_TRACE_INSTANCE_H

#include "smart_io_types.h"

int smart_io_trace_instance_init(void);
void smart_io_trace_instance_exit(void);
void smart_io_trace_emit_complete(const struct smart_io_event *evt);
void smart_io_raw_emit(const char *fmt, ...);
void smart_io_trace_debug(const char *fmt, ...);
void smart_io_trace_log(const char *fmt, ...);

#endif
