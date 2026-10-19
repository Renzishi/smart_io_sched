/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Offline learned-policy decision table for MODEL mode (doc §Phase 3).
 *
 * The IQL pi-head (3x128 trunk + 2-dim action head) is too large to run in
 * the kernel hot path. Instead, the policy is exported offline as a DENSE
 * decision table: for a fixed subset of kernel-indexable state dimensions,
 * each cell stores one bit (0=KEEP, 1=ELEVATE). The kernel computes a flat
 * index from the current smart_io_rl_state using integer math only and
 * performs an O(1) table lookup.
 *
 * GKI constraint: the module must NOT open device files (filp_open is not an
 * exported GKI symbol). So the table is STREAMED in from userspace through a
 * procfs write: `cat /data/.../policy.bin > /proc/smart_io_sched/rl_policy_bin`.
 * The first 35 bytes form the header; the remainder fills the table buffer.
 *
 * Binary layout (little-endian, packed) produced by export_policy_table.py:
 *   off 0  u8  version (must be 1)
 *   off 1  u8  n_dims (1..25)
 *   off 2  u8  bases[25]  (base per dim in GROW_ORDER, 2..32; unused = 0)
 *   off 27 u32 table_size (= prod of the first n_dims bases)
 *   off 31 u32 app_uid    (fg uid this table applies to; 0 = any)
 *   off 35 table_size bytes (0=KEEP, 1=ELEVATE)
 *
 * Fallback contract: table not loaded / not enabled / uid mismatch / index
 * out of bounds -> KEEP (the safe baseline path).
 */
#ifndef SMART_IO_RL_POLICY_H
#define SMART_IO_RL_POLICY_H

#include <linux/types.h>
#include "smart_io_rl.h"

#define SMART_IO_POLICY_VERSION  1U
#define SMART_IO_POLICY_MAX_DIMS 25U
#define SMART_IO_POLICY_MAX_BASE 32U
#define SMART_IO_POLICY_MAX_SIZE (1U << 20) /* 1 MiB */

struct smart_io_rl_policy {
	bool loaded;
	bool enabled;
	u8  version;
	u8  n_dims;
	u8  bases[SMART_IO_POLICY_MAX_DIMS];
	u32 table_size;
	u32 app_uid;    /* 0 = applies to any fg uid */
	u8  *table;     /* vmalloc'd; freed on reload / module exit */
	/* Stats */
	u64 hits;
	u64 misses;     /* not-loaded / uid-mismatch / index OOB */
};

/*
 * Stage table bytes (process context, procfs write). The first 35 bytes are
 * the header (validated + table buffer allocated); the rest fill the buffer.
 * When the buffer is complete, the table is published atomically (old freed).
 * Returns 0 on success, -EINVAL on a malformed header, -ENOMEM on allocation
 * failure.
 */
int smart_io_rl_policy_feed(const u8 *data, u32 len);

/* Reset the staging state AND any loaded table (used before a re-feed or at
 * module exit). */
void smart_io_rl_policy_reset(void);

/* Enable / disable the loaded table (does not free it). */
void smart_io_rl_policy_set_enabled(bool on);

/* Try to decide an action from the loaded table.
 * Returns 0 and sets *action on a hit; returns -EOPNOTSUPP (caller falls
 * back to KEEP) on any miss (not loaded, not enabled, uid mismatch, index
 * OOB). Hot path: no sleep, no alloc, no lock. */
int smart_io_rl_policy_decide(const struct smart_io_rl_state *state,
			      u32 fg_uid, u8 *action);

/* Status for /proc/smart_io_sched/rl_policy_status. */
void smart_io_rl_policy_status(u8 *version, u8 *n_dims, u32 *table_size,
			       u32 *app_uid, bool *loaded, bool *enabled,
			       u64 *hits, u64 *misses);

/* Free the table and staging state (module exit). */
void smart_io_rl_policy_free(void);

#endif
