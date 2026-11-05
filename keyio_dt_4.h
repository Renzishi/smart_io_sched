/* SPDX-License-Identifier: GPL-2.0 */
/*
 * keyio_dt_4.h - depth-4 distilled decision tree for critical-IO (keyio)
 *                classification at waker insert time.
 *
 * The tree distills the hard decision of the final DNN model (8 sessions,
 * 25,133,262 rows, insert-CPU feature set) at its F1-max threshold
 * (0.6616). Test-set agreement with the DNN: 0.873; at the F1-max
 * reference point the tree scores P=0.396 / R=0.340.
 *
 * All inputs are available on the block insert path without allocation,
 * sleeping or locking:
 *   runqueue  - sum of nr_running over online CPUs (get_runqueue_len())
 *   main_is_s - fg main thread in TASK_INTERRUPTIBLE (smart_io_rl_main_in_s())
 *   ioprio    - req_get_ioprio()
 *   comm      - get_task_comm(current); cleaned and n-grammed below
 *   file_ext  - module file-ext code (0 UNKNOWN / 1 DB / ...)
 *   fg_uid    - current foreground app uid (smart_io_get_fg_ctx())
 *   cpu_id    - smp_processor_id()
 *
 * comm n-gram features reproduce the offline pipeline exactly:
 *   clean: strip ONE trailing "-<digits>", then replace every digit run
 *          with a single '#'
 *   feats: for n in {3,4}: for each n-gram g: bucket = crc32(g) % 64
 *          (zlib.crc32 == kernel crc32_le(0, g, n))
 *          feature_k = floor(count(bucket k) * 1e6 / total_grams)
 * Only buckets 11/13/26/53 are needed by this tree.
 *
 * App one-hot semantics: the offline app bit marks the foreground scene
 * app, so in-kernel it is derived from fg_uid (the foreground app uid),
 * not from the requesting thread's uid. Uids below are the per-app
 * foreground uids of the training device (8 sessions, 20261022-20261027);
 * they are device-specific - refresh them when the training set changes.
 */

#ifndef _KEYIO_DT_4_H
#define _KEYIO_DT_4_H

#include <linux/types.h>
#include <linux/sched.h> /* TASK_COMM_LEN */
#include <linux/crc32.h> /* crc32_le (EXPORT_SYMBOL) */

/* Foreground-app uids used by the tree (app1=douyin, app2=feishu,
 * app4=tiktok_Asia). */
#define KEYIO_DT4_UID_DOUYIN      10275
#define KEYIO_DT4_UID_FEISHU      10271
#define KEYIO_DT4_UID_TIKTOK_ASIA 10276

/* n-gram buckets (of 64) used by the tree */
#define KEYIO_DT4_BUCKET_11 11
#define KEYIO_DT4_BUCKET_13 13
#define KEYIO_DT4_BUCKET_26 26
#define KEYIO_DT4_BUCKET_53 53

/*
 * Clean a comm string the way the offline pipeline does:
 *   re.sub(r"-\d+$", "", comm)  then  re.sub(r"\d+", "#", comm)
 * Byte-wise, ASCII digits. comm is NUL-terminated (TASK_COMM_LEN).
 */
static inline void keyio_dt4_clean_name(const char *comm, char *out)
{
	size_t len = 0, i, o = 0;
	const char *p;

	while (len < TASK_COMM_LEN - 1 && comm[len])
		len++;
	p = comm;

	/* Strip one trailing "-<digits>" run (only if a '-' precedes it). */
	{
		size_t e = len;

		while (e > 0 && p[e - 1] >= '0' && p[e - 1] <= '9')
			e--;
		if (e < len && e > 0 && p[e - 1] == '-')
			len = e - 1;
	}

	/* Replace every digit run with a single '#'. */
	for (i = 0; i < len; i++) {
		if (p[i] >= '0' && p[i] <= '9') {
			out[o++] = '#';
			while (i + 1 < len && p[i + 1] >= '0' &&
			       p[i + 1] <= '9')
				i++;
		} else {
			out[o++] = p[i];
		}
	}
	out[o] = '\0';
}

/*
 * Compute the four tree n-gram features of a cleaned name:
 *   fK = floor(count(bucket K) * 1e6 / total_grams), K in 11/13/26/53
 * total_grams = (len-2) + (len-3) = 2*len-5 (0 when len < 3).
 * All-zero when len < 3, matching the offline pipeline.
 */
static inline void keyio_dt4_name_feats(const char *s, u32 *f11, u32 *f13,
					u32 *f26, u32 *f53)
{
	size_t len = 0, i;
	u32 c11 = 0, c13 = 0, c26 = 0, c53 = 0, tot;
	u32 b;

	*f11 = *f13 = *f26 = *f53 = 0;
	while (s[len])
		len++;
	if (len < 3)
		return;

	for (i = 0; i + 3 <= len; i++) {
		b = crc32_le(0, s + i, 3) % 64;
		if (b == KEYIO_DT4_BUCKET_11)
			c11++;
		else if (b == KEYIO_DT4_BUCKET_13)
			c13++;
		else if (b == KEYIO_DT4_BUCKET_26)
			c26++;
		else if (b == KEYIO_DT4_BUCKET_53)
			c53++;
	}
	for (i = 0; i + 4 <= len; i++) {
		b = crc32_le(0, s + i, 4) % 64;
		if (b == KEYIO_DT4_BUCKET_11)
			c11++;
		else if (b == KEYIO_DT4_BUCKET_13)
			c13++;
		else if (b == KEYIO_DT4_BUCKET_26)
			c26++;
		else if (b == KEYIO_DT4_BUCKET_53)
			c53++;
	}

	tot = (u32)(2 * len - 5);
	*f11 = (u32)((u64)c11 * 1000000 / tot);
	*f13 = (u32)((u64)c13 * 1000000 / tot);
	*f26 = (u32)((u64)c26 * 1000000 / tot);
	*f53 = (u32)((u64)c53 * 1000000 / tot);
}

/* Tree node: internal nodes compare feat value <= thr, leaves carry cls.
 * thr units: x1e6 fixed-point for name feats, raw integer otherwise
 * (runqueue/ioprio/file_ext are integer counts; binary feats use thr 0,
 * i.e. value == 0 goes left). */
enum keyio_dt4_feat {
	KDT4F_RUNQUEUE,
	KDT4F_MAIN_IS_S,
	KDT4F_IOPRIO,
	KDT4F_NAME11,
	KDT4F_NAME13,
	KDT4F_NAME26,
	KDT4F_NAME53,
	KDT4F_FILE_EXT,
	KDT4F_APP1,
	KDT4F_APP2,
	KDT4F_APP4,
	KDT4F_CPU6,
	KDT4F_NONE
};

struct keyio_dt4_node {
	u8 feat;
	u8 left;
	u8 right;
	s32 thr;
	s8 cls;
};

/*
 * The depth-4 tree (31 nodes). Thresholds are the fitted sklearn
 * thresholds converted to raw units with the exact per-feature train
 * mean/std; integer features are exact (thresholds are x.5 midpoints),
 * name feats use floor(raw * 1e6).
 * Positive leaves: 5, 7, 14 (leaf 23 is unreachable: app1 and app4
 * are one-hot, so both can never be 1).
 */
static const struct keyio_dt4_node keyio_dt4_nodes[31] = {
	[0]  = { KDT4F_RUNQUEUE,  1, 16, 11,     0 },
	[1]  = { KDT4F_MAIN_IS_S, 2,  9, 0,      0 },
	[2]  = { KDT4F_IOPRIO,    3,  6, 10273,  0 },
	[3]  = { KDT4F_NAME13,    4,  5, 50125,  0 },
	[4]  = { KDT4F_NONE,      0,  0, 0,      0 },
	[5]  = { KDT4F_NONE,      0,  0, 0,      1 },
	[6]  = { KDT4F_NAME26,    7,  8, 50125,  0 },
	[7]  = { KDT4F_NONE,      0,  0, 0,      1 },
	[8]  = { KDT4F_NONE,      0,  0, 0,      0 },
	[9]  = { KDT4F_NAME53,   10, 13, 108187, 0 },
	[10] = { KDT4F_APP2,     11, 12, 0,      0 },
	[11] = { KDT4F_NONE,     0,  0, 0,      0 },
	[12] = { KDT4F_NONE,     0,  0, 0,      0 },
	[13] = { KDT4F_FILE_EXT, 14, 15, 1,      0 },
	[14] = { KDT4F_NONE,     0,  0, 0,      1 },
	[15] = { KDT4F_NONE,     0,  0, 0,      0 },
	[16] = { KDT4F_APP1,     17, 24, 0,      0 },
	[17] = { KDT4F_NAME11,   18, 21, 19999,  0 },
	[18] = { KDT4F_RUNQUEUE, 19, 20, 14,     0 },
	[19] = { KDT4F_NONE,     0,  0, 0,      0 },
	[20] = { KDT4F_NONE,     0,  0, 0,      0 },
	[21] = { KDT4F_APP4,     22, 23, 0,      0 },
	[22] = { KDT4F_NONE,     0,  0, 0,      0 },
	[23] = { KDT4F_NONE,     0,  0, 0,      1 },
	[24] = { KDT4F_RUNQUEUE, 25, 28, 17,     0 },
	[25] = { KDT4F_MAIN_IS_S, 26, 27, 0,     0 },
	[26] = { KDT4F_NONE,     0,  0, 0,      0 },
	[27] = { KDT4F_NONE,     0,  0, 0,      0 },
	[28] = { KDT4F_CPU6,     29, 30, 0,      0 },
	[29] = { KDT4F_NONE,     0,  0, 0,      0 },
	[30] = { KDT4F_NONE,     0,  0, 0,      0 },
};

struct keyio_dt4_inputs {
	u32 runqueue;
	u8 main_is_s;
	u16 ioprio;
	u16 file_ext;
	u8 app1;
	u8 app2;
	u8 app4;
	u8 cpu6;
	u32 name11_x1e6;
	u32 name13_x1e6;
	u32 name26_x1e6;
	u32 name53_x1e6;
};

static inline u8 keyio_dt4_decide(const struct keyio_dt4_inputs *in)
{
	int node = 0;

	for (;;) {
		const struct keyio_dt4_node *nd = &keyio_dt4_nodes[node];
		u32 v;

		switch (nd->feat) {
		case KDT4F_RUNQUEUE:  v = in->runqueue; break;
		case KDT4F_MAIN_IS_S: v = in->main_is_s; break;
		case KDT4F_IOPRIO:    v = in->ioprio; break;
		case KDT4F_FILE_EXT:  v = in->file_ext; break;
		case KDT4F_APP1:      v = in->app1; break;
		case KDT4F_APP2:      v = in->app2; break;
		case KDT4F_APP4:      v = in->app4; break;
		case KDT4F_CPU6:      v = in->cpu6; break;
		case KDT4F_NAME11:    v = in->name11_x1e6; break;
		case KDT4F_NAME13:    v = in->name13_x1e6; break;
		case KDT4F_NAME26:    v = in->name26_x1e6; break;
		case KDT4F_NAME53:    v = in->name53_x1e6; break;
		default:             return nd->cls ? 1 : 0;
		}
		node = (v <= (u32)nd->thr) ? nd->left : nd->right;
	}
}

/*
 * Full prediction: 1 = critical (the DNN would cross its F1-max
 * threshold), 0 = not critical.
 */
static inline u8 keyio_dt4_predict(u32 runqueue, u8 main_is_s, u16 ioprio,
				   const char *comm, u16 file_ext,
				   u32 fg_uid, u32 cpu_id)
{
	struct keyio_dt4_inputs in;
	char cleaned[TASK_COMM_LEN];

	in.runqueue = runqueue;
	in.main_is_s = main_is_s ? 1 : 0;
	in.ioprio = ioprio;
	in.file_ext = file_ext;
	in.cpu6 = (cpu_id == 6) ? 1 : 0;
	in.app1 = (fg_uid == KEYIO_DT4_UID_DOUYIN) ? 1 : 0;
	in.app2 = (fg_uid == KEYIO_DT4_UID_FEISHU) ? 1 : 0;
	in.app4 = (fg_uid == KEYIO_DT4_UID_TIKTOK_ASIA) ? 1 : 0;

	keyio_dt4_clean_name(comm, cleaned);
	keyio_dt4_name_feats(cleaned, &in.name11_x1e6, &in.name13_x1e6,
			     &in.name26_x1e6, &in.name53_x1e6);

	return keyio_dt4_decide(&in);
}

#endif /* _KEYIO_DT_4_H */
