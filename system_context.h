#ifndef SYSTEM_CONTEXT_H
#define SYSTEM_CONTEXT_H

#include <linux/types.h>

int smart_io_ctx_init(void);
void smart_io_ctx_exit(void);
void smart_io_set_fg_uid(uid_t uid);
int smart_io_get_fg_uid(void);
void smart_io_set_scene_tag(u32 scene_tag);
void smart_io_get_fg_ctx(uid_t *uid, u32 *scene_tag);
void smart_io_set_fg_main_pid(pid_t pid);
int smart_io_get_fg_main_pid(void);

#endif
