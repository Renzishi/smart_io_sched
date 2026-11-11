/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * keyio_dt_v5.h - depth-8 distilled tree for critical-IO prediction.
 *
 * Teacher : MLP[79,64,32,8,1] on the NON-main-thread population of
 *           dataset_v5, trained on is_critical_r1r2.
 *           val_auc=0.9826 test_auc=0.9752 F1max=0.8533 (thr=0.918568)
 * Student : DecisionTree(max_depth=8, 137 nodes), agreement 0.990252,
 *           true P=0.9545 R=0.7194 F1=0.8205
 *
 * Inputs are exactly what the kernel can compute at insert time.
 * Thresholds are already in kernel units: io_size in bytes, name
 * features as count*1e6/(2*len-5) truncated to an integer.
 */
#ifndef _KEYIO_DT_V5_H
#define _KEYIO_DT_V5_H

#include <linux/ctype.h>
#include <linux/crc32.h>
#include <linux/sched.h>
#include <linux/types.h>

#define KEYIO_DTV5_NBUCKETS 11u

static const u8 keyio_dtv5_buckets[KEYIO_DTV5_NBUCKETS] = {
	0, 3, 14, 16, 18, 19, 22, 32, 33, 39, 63,
};

enum keyio_dtv5_feat {
	KDTV5_IO_SIZE, /* io_size_bytes */
	KDTV5_OP, /* op */
	KDTV5_IOPRIO, /* ioprio */
	KDTV5_RUNQUEUE, /* runqueue */
	KDTV5_FILE_EXT, /* file_ext */
	KDTV5_MAIN_IS_S, /* main_is_s */
	KDTV5_UNDER_FG_UID, /* under_fg_uid */
	KDTV5_CPU, /* arg = cpu index */
	KDTV5_NAME, /* arg = bucket position */
	KDTV5_UID, /* requester uid */
	KDTV5_NONE, /* leaf */
};

struct keyio_dtv5_node {
	u8 feat;
	u8 arg;
	u16 left;
	u16 right;
	u32 thr;
	s8 cls;
};

static const struct keyio_dtv5_node keyio_dtv5_nodes[137] = {
	[  0] = { KDTV5_NAME,            10,   1,  94,     100250, 0 },
	[  1] = { KDTV5_IOPRIO,           0,   2,  89,      17156, 0 },
	[  2] = { KDTV5_NAME,             0,   3,  44,     188235, 0 },
	[  3] = { KDTV5_NAME,             6,   4,  33,      88932, 0 },
	[  4] = { KDTV5_NAME,             2,   5,  18,      83478, 0 },
	[  5] = { KDTV5_NAME,             3,   6,  13,     118823, 0 },
	[  6] = { KDTV5_MAIN_IS_S,        0,   7,  10,          0, 0 },
	[  7] = { KDTV5_OP,               0,   8,   9,          0, 0 },
	[  8] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[  9] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 10] = { KDTV5_NAME,             4,  11,  12,      78461, 0 },
	[ 11] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 12] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 13] = { KDTV5_MAIN_IS_S,        0,  14,  15,          0, 0 },
	[ 14] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 15] = { KDTV5_NAME,             5,  16,  17,      20000, 0 },
	[ 16] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 17] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 18] = { KDTV5_NAME,             8,  19,  26,      21739, 0 },
	[ 19] = { KDTV5_NAME,             9,  20,  23,     136363, 0 },
	[ 20] = { KDTV5_OP,               0,  21,  22,          0, 0 },
	[ 21] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 22] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 23] = { KDTV5_RUNQUEUE,         0,  24,  25,          8, 0 },
	[ 24] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 25] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 26] = { KDTV5_RUNQUEUE,         0,  27,  30,          9, 0 },
	[ 27] = { KDTV5_MAIN_IS_S,        0,  28,  29,          0, 0 },
	[ 28] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 29] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 30] = { KDTV5_RUNQUEUE,         0,  31,  32,         11, 0 },
	[ 31] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 32] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 33] = { KDTV5_FILE_EXT,         0,  34,  43,          3, 0 },
	[ 34] = { KDTV5_MAIN_IS_S,        0,  35,  36,          0, 0 },
	[ 35] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 36] = { KDTV5_CPU,              6,  37,  40,          0, 0 },
	[ 37] = { KDTV5_CPU,              7,  38,  39,          0, 0 },
	[ 38] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 39] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 40] = { KDTV5_UID,              0,  41,  42,      10223, 0 },
	[ 41] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 42] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 43] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 44] = { KDTV5_MAIN_IS_S,        0,  45,  60,          0, 0 },
	[ 45] = { KDTV5_IOPRIO,           0,  46,  59,      10624, 0 },
	[ 46] = { KDTV5_RUNQUEUE,         0,  47,  52,          5, 0 },
	[ 47] = { KDTV5_FILE_EXT,         0,  48,  51,          1, 0 },
	[ 48] = { KDTV5_CPU,              0,  49,  50,          0, 0 },
	[ 49] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 50] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 51] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 52] = { KDTV5_RUNQUEUE,         0,  53,  56,          7, 0 },
	[ 53] = { KDTV5_FILE_EXT,         0,  54,  55,          1, 0 },
	[ 54] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 55] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 56] = { KDTV5_OP,               0,  57,  58,          0, 0 },
	[ 57] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 58] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 59] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 60] = { KDTV5_FILE_EXT,         0,  61,  74,          3, 0 },
	[ 61] = { KDTV5_RUNQUEUE,         0,  62,  67,         12, 0 },
	[ 62] = { KDTV5_RUNQUEUE,         0,  63,  64,         10, 0 },
	[ 63] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 64] = { KDTV5_CPU,              1,  65,  66,          0, 0 },
	[ 65] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 66] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 67] = { KDTV5_FILE_EXT,         0,  68,  71,          1, 0 },
	[ 68] = { KDTV5_RUNQUEUE,         0,  69,  70,         31, 0 },
	[ 69] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 70] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 71] = { KDTV5_CPU,              7,  72,  73,          0, 0 },
	[ 72] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 73] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 74] = { KDTV5_RUNQUEUE,         0,  75,  82,          8, 0 },
	[ 75] = { KDTV5_IO_SIZE,          0,  76,  79,      14188, 0 },
	[ 76] = { KDTV5_CPU,              5,  77,  78,          0, 0 },
	[ 77] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 78] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 79] = { KDTV5_RUNQUEUE,         0,  80,  81,          6, 0 },
	[ 80] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 81] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 82] = { KDTV5_RUNQUEUE,         0,  83,  86,          9, 0 },
	[ 83] = { KDTV5_IO_SIZE,          0,  84,  85,       9158, 0 },
	[ 84] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 85] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 86] = { KDTV5_CPU,              6,  87,  88,          0, 0 },
	[ 87] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 88] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 89] = { KDTV5_NAME,             7,  90,  91,      21739, 0 },
	[ 90] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 91] = { KDTV5_FILE_EXT,         0,  92,  93,          1, 0 },
	[ 92] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 93] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 94] = { KDTV5_MAIN_IS_S,        0,  95,  98,          0, 0 },
	[ 95] = { KDTV5_OP,               0,  96,  97,          0, 0 },
	[ 96] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[ 97] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[ 98] = { KDTV5_NAME,             1,  99, 106,      26315, 0 },
	[ 99] = { KDTV5_RUNQUEUE,         0, 100, 105,          4, 0 },
	[100] = { KDTV5_CPU,              1, 101, 102,          0, 0 },
	[101] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[102] = { KDTV5_FILE_EXT,         0, 103, 104,          1, 0 },
	[103] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[104] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[105] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[106] = { KDTV5_UID,              0, 107, 108,      10240, 0 },
	[107] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[108] = { KDTV5_UID,              0, 109, 122,      10275, 0 },
	[109] = { KDTV5_CPU,              3, 110, 117,          0, 0 },
	[110] = { KDTV5_RUNQUEUE,         0, 111, 114,         14, 0 },
	[111] = { KDTV5_IO_SIZE,          0, 112, 113,      38858, 0 },
	[112] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[113] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[114] = { KDTV5_IO_SIZE,          0, 115, 116,      30651, 0 },
	[115] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[116] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[117] = { KDTV5_IO_SIZE,          0, 118, 119,      20066, 0 },
	[118] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[119] = { KDTV5_RUNQUEUE,         0, 120, 121,          8, 0 },
	[120] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[121] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[122] = { KDTV5_IO_SIZE,          0, 123, 130,      42959, 0 },
	[123] = { KDTV5_FILE_EXT,         0, 124, 127,          3, 0 },
	[124] = { KDTV5_CPU,              5, 125, 126,          0, 0 },
	[125] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[126] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
	[127] = { KDTV5_FILE_EXT,         0, 128, 129,         11, 0 },
	[128] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[129] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[130] = { KDTV5_RUNQUEUE,         0, 131, 134,         11, 0 },
	[131] = { KDTV5_RUNQUEUE,         0, 132, 133,         10, 0 },
	[132] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[133] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[134] = { KDTV5_CPU,              5, 135, 136,          0, 0 },
	[135] = { KDTV5_NONE, 0, 0, 0, 0, 1 },
	[136] = { KDTV5_NONE, 0, 0, 0, 0, 0 },
};

struct keyio_dtv5_inputs {
	u32 io_size_bytes;
	u8 op;
	u16 ioprio;
	u32 runqueue;
	u16 file_ext;
	u8 main_is_s;
	u8 under_fg_uid;
	u32 uid;
	u32 cpu_id;
	u32 name[KEYIO_DTV5_NBUCKETS];
};

/*
 * Clean a comm the way the offline pipeline does:
 *   re.sub(r"-\d+$", "", comm) then re.sub(r"\d+", "#", comm)
 */
static inline void keyio_dtv5_clean_name(const char *comm, char *out)
{
	size_t len = 0, i, o = 0;
	const char *p = comm;

	while (len < TASK_COMM_LEN - 1 && comm[len])
		len++;

	{
		size_t e = len;

		while (e > 0 && p[e - 1] >= '0' && p[e - 1] <= '9')
			e--;
		if (e < len && e > 0 && p[e - 1] == '-')
			len = e - 1;
	}

	for (i = 0; i < len; i++) {
		if (p[i] >= '0' && p[i] <= '9') {
			out[o++] = '#';
			while (i + 1 < len && p[i + 1] >= '0' && p[i + 1] <= '9')
				i++;
		} else {
			out[o++] = p[i];
		}
	}
	out[o] = '\0';
}

/*
 * n-gram features for the buckets this tree uses:
 *   value = count * 1000000 / (2*len - 5), 0 when len < 3
 */
static inline void keyio_dtv5_name_feats(const char *s, u32 *out)
{
	size_t len = 0, i;
	u32 cnt[KEYIO_DTV5_NBUCKETS];
	u32 tot, b, k;

	for (k = 0; k < KEYIO_DTV5_NBUCKETS; k++) {
		cnt[k] = 0;
		out[k] = 0;
	}
	while (s[len])
		len++;
	if (len < 3)
		return;

	for (i = 0; i + 3 <= len; i++) {
		b = crc32_le(0, s + i, 3) % 64;
		for (k = 0; k < KEYIO_DTV5_NBUCKETS; k++)
			if (b == keyio_dtv5_buckets[k])
				cnt[k]++;
	}
	for (i = 0; i + 4 <= len; i++) {
		b = crc32_le(0, s + i, 4) % 64;
		for (k = 0; k < KEYIO_DTV5_NBUCKETS; k++)
			if (b == keyio_dtv5_buckets[k])
				cnt[k]++;
	}

	tot = (u32)(2 * len - 5);
	for (k = 0; k < KEYIO_DTV5_NBUCKETS; k++)
		out[k] = (u32)((u64)cnt[k] * 1000000 / tot);
}

static inline u8 keyio_dtv5_decide(const struct keyio_dtv5_inputs *in)
{
	int node = 0;

	for (;;) {
		const struct keyio_dtv5_node *nd = &keyio_dtv5_nodes[node];
		u32 v;

		switch (nd->feat) {

		case KDTV5_IO_SIZE:   v = in->io_size_bytes; break;
		case KDTV5_OP: v = in->op; break;
		case KDTV5_IOPRIO: v = in->ioprio; break;
		case KDTV5_RUNQUEUE: v = in->runqueue; break;
		case KDTV5_FILE_EXT: v = in->file_ext; break;
		case KDTV5_MAIN_IS_S: v = in->main_is_s; break;
		case KDTV5_UNDER_FG_UID: v = in->under_fg_uid; break;
		case KDTV5_UID:       v = in->uid; break;
		case KDTV5_CPU:       v = (in->cpu_id == nd->arg) ? 1 : 0; break;
		case KDTV5_NAME:      v = in->name[nd->arg]; break;
		default:              return nd->cls ? 1 : 0;
		}
		node = (v <= nd->thr) ? nd->left : nd->right;
	}
}

/*
 * Full prediction: 1 = the teacher DNN would cross its F1-max threshold.
 */
static inline u8 keyio_dtv5_predict(u32 io_size_bytes, u8 op, u16 ioprio,
				    u32 runqueue, u16 file_ext, u8 main_is_s,
				    u8 under_fg_uid, const char *comm, u32 cpu_id,
				    u32 uid)
{
	struct keyio_dtv5_inputs in;
	char cleaned[TASK_COMM_LEN];


	in.io_size_bytes = io_size_bytes;
	in.op = op;
	in.ioprio = ioprio;
	in.runqueue = runqueue;
	in.file_ext = file_ext;
	in.main_is_s = main_is_s ? 1 : 0;
	in.under_fg_uid = under_fg_uid ? 1 : 0;
	in.uid = uid;
	in.cpu_id = cpu_id;

	keyio_dtv5_clean_name(comm, cleaned);
	keyio_dtv5_name_feats(cleaned, in.name);

	return keyio_dtv5_decide(&in);
}

#endif /* _KEYIO_DT_V5_H */
