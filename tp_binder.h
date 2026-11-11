#ifndef TP_BINDER_H
#define TP_BINDER_H

#include <linux/types.h>

int register_binder_tracepoints(void);
void unregister_binder_tracepoints(void);
bool smart_io_binder_pi_current_marked(void);
/* 1 while the fg main thread has an in-flight synchronous binder call. */
bool smart_io_main_in_binder(void);
void smart_io_binder_pi_clear_all(const char *reason);

#endif
