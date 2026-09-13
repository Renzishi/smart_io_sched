#ifndef SMART_IO_TP_PI_H
#define SMART_IO_TP_PI_H

#include <linux/types.h>

struct task_struct;

int register_pi_tracepoints(void);
void unregister_pi_tracepoints(void);
bool smart_io_pi_current_marked(void);
void smart_io_pi_clear_owner(const char *reason);

#endif
