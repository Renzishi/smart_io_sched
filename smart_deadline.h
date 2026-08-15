/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMART_DEADLINE_H
#define SMART_DEADLINE_H

#include <linux/types.h>

struct request;

int smart_deadline_init(void);
void smart_deadline_exit(void);
bool smart_deadline_rq_is_foreground(const struct request *rq);

#endif
