// SPDX-License-Identifier: Apache-2.0
/* Fixed-window Android AArch64 I/O guard intervention test. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_DIRECT
#define O_DIRECT 040000
#endif

#ifndef O_DSYNC
#define O_DSYNC 00010000
#endif

#ifndef __NR_ioprio_set
#if defined(__aarch64__)
#define __NR_ioprio_set 30
#else
#error "This test is intended for Android AArch64"
#endif
#endif

#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_BE 2
#define IOPRIO_CLASS_IDLE 3
#define IOPRIO_PRIO_VALUE(class, data) (((class) << 13) | (data))

#define ALIGNMENT 4096U
#define BLOCK_BYTES 4096U
#define FOREGROUND_WORKERS 1U
#define BASELINE_BACKGROUND_WORKERS 4U
#define MAX_WORKERS 9U
#define INITIAL_RECORD_CAPACITY 65536U
#define START_LEAD_NS 200000000ULL
#define MAX_WINDOW_MS 10000U
#define BACKGROUND_GATE_DEADLINE_NS 2000000000ULL

enum guard_profile {
	PROFILE_BASELINE,
	PROFILE_READ_GUARD,
	PROFILE_WRITE_GUARD,
	PROFILE_ALL_GUARD,
	PROFILE_READ_D2,
	PROFILE_WRITE_D2,
	PROFILE_ALL_D2,
};

enum io_role {
	ROLE_FOREGROUND_READ,
	ROLE_BACKGROUND_READ,
	ROLE_BACKGROUND_WRITE,
};

enum sample_phase {
	PHASE_PRE,
	PHASE_TREATMENT,
};

struct io_record {
	uint64_t start_ns;
	uint64_t end_ns;
	uint64_t offset_bytes;
	uint64_t gate_wait_ns;
	enum sample_phase phase;
	ssize_t ret;
	uint32_t sequence;
	int saved_errno;
};

struct run_config {
	const char *read_path;
	const char *write_path;
	const char *csv_path;
	enum guard_profile profile;
	uint64_t file_bytes;
	uint64_t seed;
	uint32_t warmup_ms;
	uint32_t pre_ms;
	uint32_t settle_ms;
	uint32_t measure_ms;
	uint32_t foreground_uid;
	uint64_t read_state_threshold_ns;
	uint64_t write_state_threshold_ns;
	bool state_mode;
	sem_t read_guard;
	sem_t write_guard;
	uint64_t load_start_ns;
	uint64_t pre_start_ns;
	uint64_t action_apply_ns;
	uint64_t measure_start_ns;
	uint64_t measure_end_ns;
};

struct worker_arg {
	struct run_config *cfg;
	enum io_role role;
	uint32_t worker;
	struct io_record *records;
	uint32_t record_count;
	uint32_t pre_record_count;
	uint32_t treatment_record_count;
	uint32_t record_capacity;
	uint32_t total_io_count;
	uint64_t max_gate_wait_ns;
	int status;
	bool truncated;
	uint32_t submit_uid;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int sleep_until_ns(uint64_t target_ns)
{
	struct timespec target = {
		.tv_sec = (time_t)(target_ns / 1000000000ULL),
		.tv_nsec = (long)(target_ns % 1000000000ULL),
	};
	int ret;

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
	} while (ret == EINTR);
	return ret ? -ret : 0;
}

static uint64_t next_random(uint64_t *state)
{
	uint64_t x = *state;

	if (!x)
		x = 0x9e3779b97f4a7c15ULL;
	x ^= x << 7;
	x ^= x >> 9;
	x ^= x << 8;
	*state = x;
	return x;
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || end == text || *end || parsed > UINT32_MAX)
		return -EINVAL;
	*value = (uint32_t)parsed;
	return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end = NULL;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || end == text || *end)
		return -EINVAL;
	*value = (uint64_t)parsed;
	return 0;
}

static int set_ioprio(uint32_t ioprio_class)
{
	int ret;

	ret = (int)syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS,
			   syscall(SYS_gettid),
			   IOPRIO_PRIO_VALUE(ioprio_class, 0));
	return ret < 0 ? -errno : 0;
}

static int set_thread_uid(uint32_t uid)
{
	long ret;

	ret = syscall(SYS_setresuid, uid, uid, uid);
	if (ret < 0)
		return -errno;
	return (uint32_t)syscall(SYS_getuid) == uid ? 0 : -EPERM;
}

static const char *profile_name(enum guard_profile profile)
{
	switch (profile) {
	case PROFILE_BASELINE:
		return "BASELINE";
	case PROFILE_READ_GUARD:
		return "READ_GUARD";
	case PROFILE_WRITE_GUARD:
		return "WRITE_GUARD";
	case PROFILE_ALL_GUARD:
		return "ALL_GUARD";
	case PROFILE_READ_D2:
		return "READ_D2";
	case PROFILE_WRITE_D2:
		return "WRITE_D2";
	case PROFILE_ALL_D2:
		return "ALL_D2";
	default:
		return "INVALID";
	}
}

static int parse_profile(const char *text, enum guard_profile *profile)
{
	if (!strcmp(text, "BASELINE"))
		*profile = PROFILE_BASELINE;
	else if (!strcmp(text, "READ_GUARD"))
		*profile = PROFILE_READ_GUARD;
	else if (!strcmp(text, "WRITE_GUARD"))
		*profile = PROFILE_WRITE_GUARD;
	else if (!strcmp(text, "ALL_GUARD"))
		*profile = PROFILE_ALL_GUARD;
	else if (!strcmp(text, "READ_D2"))
		*profile = PROFILE_READ_D2;
	else if (!strcmp(text, "WRITE_D2"))
		*profile = PROFILE_WRITE_D2;
	else if (!strcmp(text, "ALL_D2"))
		*profile = PROFILE_ALL_D2;
	else
		return -EINVAL;
	return 0;
}

static uint32_t profile_read_limit(enum guard_profile profile)
{
	switch (profile) {
	case PROFILE_READ_GUARD:
	case PROFILE_ALL_GUARD:
		return 1;
	case PROFILE_READ_D2:
	case PROFILE_ALL_D2:
		return 2;
	default:
		return BASELINE_BACKGROUND_WORKERS;
	}
}

static uint32_t profile_write_limit(enum guard_profile profile)
{
	switch (profile) {
	case PROFILE_WRITE_GUARD:
	case PROFILE_ALL_GUARD:
		return 1;
	case PROFILE_WRITE_D2:
	case PROFILE_ALL_D2:
		return 2;
	default:
		return BASELINE_BACKGROUND_WORKERS;
	}
}

static bool profile_is_depth2(enum guard_profile profile)
{
	return profile == PROFILE_READ_D2 || profile == PROFILE_WRITE_D2 ||
	       profile == PROFILE_ALL_D2;
}

static const char *role_name(enum io_role role)
{
	switch (role) {
	case ROLE_FOREGROUND_READ:
		return "foreground_read";
	case ROLE_BACKGROUND_READ:
		return "background_read";
	case ROLE_BACKGROUND_WRITE:
		return "background_write";
	default:
		return "invalid";
	}
}

static bool role_is_write(enum io_role role)
{
	return role == ROLE_BACKGROUND_WRITE;
}

static const char *phase_name(enum sample_phase phase)
{
	return phase == PHASE_PRE ? "pre" : "treatment";
}

static int open_direct(const char *path, bool write)
{
	int flags = O_RDWR | O_CLOEXEC | O_DIRECT;
	int fd;

	if (write)
		flags |= O_DSYNC;
	fd = open(path, flags, 0600);
	if (fd < 0)
		fprintf(stderr, "open %s failed: errno=%d (%s)\n", path,
			errno, strerror(errno));
	return fd;
}

static int prepare_one_file(const char *path, uint64_t total_bytes,
			    unsigned char pattern)
{
	const uint32_t chunk_bytes = 1024U * 1024U;
	unsigned char *buffer = NULL;
	uint64_t offset;
	int fd = -1;
	int ret = -1;

	if (posix_memalign((void **)&buffer, ALIGNMENT, chunk_bytes))
		return -ENOMEM;
	for (uint32_t i = 0; i < chunk_bytes; i++)
		buffer[i] = (unsigned char)(pattern + i * 131U);
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC | O_DIRECT |
			O_DSYNC, 0600);
	if (fd < 0) {
		fprintf(stderr, "prepare open %s failed: errno=%d (%s)\n", path,
			errno, strerror(errno));
		goto out;
	}
	if (ftruncate(fd, (off_t)total_bytes) != 0) {
		fprintf(stderr, "ftruncate %s failed: errno=%d (%s)\n", path,
			errno, strerror(errno));
		goto out;
	}
	for (offset = 0; offset < total_bytes; offset += chunk_bytes) {
		ssize_t written = pwrite(fd, buffer, chunk_bytes, (off_t)offset);

		if (written != (ssize_t)chunk_bytes) {
			fprintf(stderr,
				"prepare pwrite %s offset=%" PRIu64
				" ret=%zd errno=%d (%s)\n",
				path, offset, written, errno, strerror(errno));
			goto out;
		}
	}
	if (fsync(fd) != 0) {
		fprintf(stderr, "fsync %s failed: errno=%d (%s)\n", path,
			errno, strerror(errno));
		goto out;
	}
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return ret;
}

static int prepare_files(const char *read_path, const char *write_path,
			 uint64_t size_mb)
{
	uint64_t total_bytes;
	int ret;

	if (!size_mb || size_mb > 1024)
		return -EINVAL;
	total_bytes = size_mb * 1024ULL * 1024ULL;
	if (total_bytes % (1024U * 1024U))
		return -EINVAL;
	ret = prepare_one_file(read_path, total_bytes, 17U);
	if (ret)
		return ret;
	return prepare_one_file(write_path, total_bytes, 83U);
}

static int append_record(struct worker_arg *arg, uint32_t sequence,
			 uint64_t offset, uint64_t gate_wait_ns,
			 uint64_t start, uint64_t end, enum sample_phase phase,
			 ssize_t io_ret, int saved_errno)
{
	struct io_record *record;

	if (arg->record_count >= arg->record_capacity) {
		arg->truncated = true;
		return -ENOSPC;
	}
	record = &arg->records[arg->record_count++];
	record->start_ns = start;
	record->end_ns = end;
	record->offset_bytes = offset;
	record->gate_wait_ns = gate_wait_ns;
	record->phase = phase;
	record->ret = io_ret;
	record->sequence = sequence;
	record->saved_errno = saved_errno;
	if (phase == PHASE_PRE)
		arg->pre_record_count++;
	else
		arg->treatment_record_count++;
	return 0;
}

static void *io_worker(void *opaque)
{
	struct worker_arg *arg = opaque;
	struct run_config *cfg = arg->cfg;
	const bool write = role_is_write(arg->role);
	const uint32_t read_limit = profile_read_limit(cfg->profile);
	const uint32_t write_limit = profile_write_limit(cfg->profile);
	const bool guard_requested =
		(arg->role == ROLE_BACKGROUND_READ &&
		 read_limit < BASELINE_BACKGROUND_WORKERS) ||
		(arg->role == ROLE_BACKGROUND_WRITE &&
		 write_limit < BASELINE_BACKGROUND_WORKERS);
	sem_t *guard = write ? &cfg->write_guard : &cfg->read_guard;
	const char *path = write ? cfg->write_path : cfg->read_path;
	uint32_t ioprio_class = arg->role == ROLE_FOREGROUND_READ ?
				IOPRIO_CLASS_BE : IOPRIO_CLASS_IDLE;
	uint64_t state = cfg->seed ^ ((uint64_t)arg->role + 1U) *
			 0xd6e8feb86659fd93ULL ^
			 ((uint64_t)arg->worker + 1U) * 0xa0761d6478bd642fULL;
	uint64_t slots = cfg->file_bytes / BLOCK_BYTES;
	unsigned char *buffer = NULL;
	uint32_t sequence = 0;
	int fd = -1;

	arg->records = calloc(INITIAL_RECORD_CAPACITY,
			      sizeof(*arg->records));
	if (!arg->records) {
		arg->status = -ENOMEM;
		return NULL;
	}
	arg->record_capacity = INITIAL_RECORD_CAPACITY;
	if (posix_memalign((void **)&buffer, ALIGNMENT, BLOCK_BYTES)) {
		arg->status = -ENOMEM;
		return NULL;
	}
	memset(buffer, (int)(31U + arg->role * 17U + arg->worker),
	       BLOCK_BYTES);
	fd = open_direct(path, write);
	if (fd < 0) {
		arg->status = -errno;
		goto out;
	}
	arg->status = set_ioprio(ioprio_class);
	if (arg->status) {
		fprintf(stderr, "role=%s worker=%u ioprio failed: %d\n",
			role_name(arg->role), arg->worker, arg->status);
		goto out;
	}
	if (arg->role == ROLE_FOREGROUND_READ) {
		arg->status = set_thread_uid(cfg->foreground_uid);
		if (arg->status) {
			fprintf(stderr, "foreground setuid failed: %d\n",
				arg->status);
			goto out;
		}
	}
	arg->submit_uid = (uint32_t)syscall(SYS_getuid);
	arg->status = sleep_until_ns(cfg->load_start_ns);
	if (arg->status)
		goto out;

	for (;;) {
		bool guard_active;
		bool record_sample = false;
		enum sample_phase phase = PHASE_TREATMENT;
		uint64_t offset;
		uint64_t gate_wait_ns;
		uint64_t gate_enter_ns;
		uint64_t start;
		uint64_t end;
		ssize_t io_ret;
		int gate_status;
		int saved_errno = 0;

		gate_enter_ns = monotonic_ns();
		guard_active = guard_requested &&
			(!cfg->state_mode || gate_enter_ns >= cfg->action_apply_ns);
		if (guard_active) {
			do {
				gate_status = sem_wait(guard) == 0 ? 0 : -errno;
			} while (gate_status == -EINTR);
			if (gate_status) {
				arg->status = gate_status;
				break;
			}
		}
		start = monotonic_ns();
		gate_wait_ns = guard_active && start > gate_enter_ns ?
			       start - gate_enter_ns : 0;
		if (gate_wait_ns > arg->max_gate_wait_ns)
			arg->max_gate_wait_ns = gate_wait_ns;
		if (gate_wait_ns >= BACKGROUND_GATE_DEADLINE_NS) {
			arg->status = -ETIMEDOUT;
			goto iteration_done;
		}
		if (start >= cfg->measure_end_ns)
			goto iteration_done;
		offset = (next_random(&state) % slots) * BLOCK_BYTES;
		if (write)
			io_ret = pwrite(fd, buffer, BLOCK_BYTES, (off_t)offset);
		else
			io_ret = pread(fd, buffer, BLOCK_BYTES, (off_t)offset);
		end = monotonic_ns();
		if (io_ret < 0)
			saved_errno = errno;
		arg->total_io_count++;
		if (cfg->state_mode && start >= cfg->pre_start_ns &&
		    end <= cfg->action_apply_ns) {
			phase = PHASE_PRE;
			record_sample = true;
		} else if (start >= cfg->measure_start_ns) {
			phase = PHASE_TREATMENT;
			record_sample = true;
		}
		if (record_sample &&
		    append_record(arg, sequence, offset, gate_wait_ns, start, end,
				  phase, io_ret, saved_errno)) {
			arg->status = -ENOSPC;
			if (guard_active)
				sem_post(guard);
			break;
		}
		sequence++;
	iteration_done:
		if (guard_active)
			sem_post(guard);
		if (start >= cfg->measure_end_ns)
			break;
	}
out:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return NULL;
}

static int write_csv(const struct run_config *cfg, struct worker_arg *args,
			     uint32_t worker_count)
{
	FILE *csv = fopen(cfg->csv_path, "w");

	if (!csv) {
		fprintf(stderr, "open CSV %s failed: errno=%d (%s)\n",
			cfg->csv_path, errno, strerror(errno));
		return -errno;
	}
	if (cfg->state_mode)
		fputs("profile,phase,read_guard,write_guard,role,op,worker,"
		      "sequence,offset_bytes,size_bytes,gate_wait_ns,start_ns,"
		      "end_ns,latency_ns,ret,errno,ioprio_class,submit_uid\n",
		      csv);
	else
		fputs("profile,read_guard,write_guard,role,op,worker,sequence,"
		      "offset_bytes,size_bytes,gate_wait_ns,start_ns,end_ns,"
		      "latency_ns,ret,errno,ioprio_class,submit_uid\n", csv);
	for (uint32_t i = 0; i < worker_count; i++) {
		struct worker_arg *arg = &args[i];
		uint32_t ioprio_class = arg->role == ROLE_FOREGROUND_READ ?
					IOPRIO_CLASS_BE : IOPRIO_CLASS_IDLE;
		for (uint32_t j = 0; j < arg->record_count; j++) {
			struct io_record *record = &arg->records[j];

			if (cfg->state_mode)
				fprintf(csv,
					"%s,%s,%u,%u,%s,%s,%u,%u,%" PRIu64
					",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
					",%" PRIu64 ",%zd,%d,%u,%u\n",
					profile_name(cfg->profile),
					phase_name(record->phase),
					profile_read_limit(cfg->profile) <
						BASELINE_BACKGROUND_WORKERS,
					profile_write_limit(cfg->profile) <
						BASELINE_BACKGROUND_WORKERS,
					role_name(arg->role),
					role_is_write(arg->role) ? "write" : "read",
					arg->worker, record->sequence,
					record->offset_bytes, BLOCK_BYTES,
					record->gate_wait_ns, record->start_ns,
					record->end_ns,
					record->end_ns - record->start_ns,
					record->ret, record->saved_errno,
					ioprio_class, arg->submit_uid);
			else
				fprintf(csv,
					"%s,%u,%u,%s,%s,%u,%u,%" PRIu64
					",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
					",%" PRIu64 ",%zd,%d,%u,%u\n",
					profile_name(cfg->profile),
					profile_read_limit(cfg->profile) <
						BASELINE_BACKGROUND_WORKERS,
					profile_write_limit(cfg->profile) <
						BASELINE_BACKGROUND_WORKERS,
					role_name(arg->role),
					role_is_write(arg->role) ? "write" : "read",
					arg->worker, record->sequence,
					record->offset_bytes, BLOCK_BYTES,
					record->gate_wait_ns, record->start_ns,
					record->end_ns,
					record->end_ns - record->start_ns,
					record->ret, record->saved_errno,
					ioprio_class, arg->submit_uid);
		}
	}
	if (fflush(csv) != 0 || fclose(csv) != 0)
		return -EIO;
	return 0;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return a > b ? 1 : a < b ? -1 : 0;
}

static int phase_role_p99(struct worker_arg *args, uint32_t worker_count,
			  enum sample_phase phase, enum io_role role,
			  uint64_t *p99_ns)
{
	uint64_t *values;
	size_t count = 0;
	size_t index = 0;
	size_t rank;

	for (uint32_t i = 0; i < worker_count; i++) {
		if (args[i].role != role)
			continue;
		for (uint32_t j = 0; j < args[i].record_count; j++) {
			if (args[i].records[j].phase == phase)
				count++;
		}
	}
	if (!count)
		return -ENODATA;
	values = malloc(count * sizeof(*values));
	if (!values)
		return -ENOMEM;
	for (uint32_t i = 0; i < worker_count; i++) {
		if (args[i].role != role)
			continue;
		for (uint32_t j = 0; j < args[i].record_count; j++) {
			struct io_record *record = &args[i].records[j];

			if (record->phase == phase)
				values[index++] = record->end_ns - record->start_ns;
		}
	}
	qsort(values, count, sizeof(*values), compare_u64);
	rank = (99U * count + 99U) / 100U;
	*p99_ns = values[rank - 1U];
	free(values);
	return 0;
}

static const char *pre_state_name(const struct run_config *cfg,
				  uint64_t read_p99_ns,
				  uint64_t write_p99_ns)
{
	bool read_slow;
	bool write_slow;

	if (!cfg->read_state_threshold_ns || !cfg->write_state_threshold_ns)
		return "UNCLASSIFIED";
	read_slow = read_p99_ns >= cfg->read_state_threshold_ns;
	write_slow = write_p99_ns >= cfg->write_state_threshold_ns;
	if (read_slow && write_slow)
		return "MIXED_SLOW";
	if (read_slow)
		return "READ_SLOW";
	if (write_slow)
		return "WRITE_SLOW";
	return "NORMAL";
}

static int run_trial(struct run_config *cfg)
{
	pthread_t threads[MAX_WORKERS];
	struct worker_arg args[MAX_WORKERS];
	uint32_t created = 0;
	uint32_t worker_count;
	uint32_t arg_index = 0;
	uint64_t pre_fg_p99_ns = 0;
	uint64_t pre_bg_read_p99_ns = 0;
	uint64_t pre_bg_write_p99_ns = 0;
	int ret = 0;
	int init_error;

	worker_count = FOREGROUND_WORKERS + 2U * BASELINE_BACKGROUND_WORKERS;
	if (worker_count > MAX_WORKERS || !cfg->file_bytes ||
	    cfg->file_bytes % BLOCK_BYTES || !cfg->warmup_ms ||
	    !cfg->measure_ms || cfg->warmup_ms > MAX_WINDOW_MS ||
	    cfg->measure_ms > MAX_WINDOW_MS ||
	    (cfg->state_mode && (!cfg->pre_ms || !cfg->settle_ms ||
				 cfg->pre_ms > MAX_WINDOW_MS ||
				 cfg->settle_ms > MAX_WINDOW_MS)) ||
	    (!cfg->state_mode && (cfg->pre_ms || cfg->settle_ms)))
		return -EINVAL;
	memset(args, 0, sizeof(args));
	if (sem_init(&cfg->read_guard, 0, profile_read_limit(cfg->profile)) != 0)
		return -errno;
	if (sem_init(&cfg->write_guard, 0, profile_write_limit(cfg->profile)) != 0) {
		init_error = -errno;
		sem_destroy(&cfg->read_guard);
		return init_error;
	}
	for (uint32_t i = 0; i < FOREGROUND_WORKERS; i++, arg_index++) {
		args[arg_index].cfg = cfg;
		args[arg_index].role = ROLE_FOREGROUND_READ;
		args[arg_index].worker = i;
	}
	for (uint32_t i = 0; i < BASELINE_BACKGROUND_WORKERS;
	     i++, arg_index++) {
		args[arg_index].cfg = cfg;
		args[arg_index].role = ROLE_BACKGROUND_READ;
		args[arg_index].worker = i;
	}
	for (uint32_t i = 0; i < BASELINE_BACKGROUND_WORKERS;
	     i++, arg_index++) {
		args[arg_index].cfg = cfg;
		args[arg_index].role = ROLE_BACKGROUND_WRITE;
		args[arg_index].worker = i;
	}

	cfg->load_start_ns = monotonic_ns() + START_LEAD_NS;
	cfg->pre_start_ns = cfg->load_start_ns +
			    (uint64_t)cfg->warmup_ms * 1000000ULL;
	if (cfg->state_mode) {
		cfg->action_apply_ns = cfg->pre_start_ns +
			(uint64_t)cfg->pre_ms * 1000000ULL;
		cfg->measure_start_ns = cfg->action_apply_ns +
			(uint64_t)cfg->settle_ms * 1000000ULL;
	} else {
		cfg->action_apply_ns = cfg->pre_start_ns;
		cfg->measure_start_ns = cfg->pre_start_ns;
	}
	cfg->measure_end_ns = cfg->measure_start_ns +
			      (uint64_t)cfg->measure_ms * 1000000ULL;
	for (uint32_t i = 0; i < worker_count; i++) {
		if (pthread_create(&threads[i], NULL, io_worker, &args[i])) {
			fprintf(stderr, "pthread_create failed for index=%u\n", i);
			ret = -EAGAIN;
			break;
		}
		created++;
	}
	for (uint32_t i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != worker_count)
		ret = -EAGAIN;
	for (uint32_t i = 0; i < created; i++) {
		if ((!args[i].record_count ||
		     (cfg->state_mode && (!args[i].pre_record_count ||
					 !args[i].treatment_record_count))) &&
		    !args[i].status)
			args[i].status = -ENODATA;
		if (args[i].status || args[i].truncated) {
			fprintf(stderr,
				"role=%s worker=%u status=%d truncated=%u\n",
				role_name(args[i].role), args[i].worker,
				args[i].status, args[i].truncated ? 1U : 0U);
			ret = args[i].status ? args[i].status : -ENOSPC;
		}
	}
	if (!ret && cfg->state_mode)
		ret = phase_role_p99(args, created, PHASE_PRE,
				     ROLE_FOREGROUND_READ, &pre_fg_p99_ns);
	if (!ret && cfg->state_mode)
		ret = phase_role_p99(args, created, PHASE_PRE,
				     ROLE_BACKGROUND_READ, &pre_bg_read_p99_ns);
	if (!ret && cfg->state_mode)
		ret = phase_role_p99(args, created, PHASE_PRE,
				     ROLE_BACKGROUND_WRITE, &pre_bg_write_p99_ns);
	if (!ret)
		ret = write_csv(cfg, args, created);
	printf("profile=%s read_guard=%u write_guard=%u fg_workers=%u "
	       "read_limit=%u write_limit=%u "
	       "bg_read_workers=%u bg_write_workers=%u state_mode=%u "
	       "warmup_ms=%u pre_ms=%u settle_ms=%u measure_ms=%u "
	       "foreground_uid=%u seed=%" PRIu64
	       " load_start_ns=%" PRIu64
	       " pre_start_ns=%" PRIu64 " action_apply_ns=%" PRIu64
	       " measure_start_ns=%" PRIu64 " measure_end_ns=%" PRIu64
	       " ret=%d\n",
	       profile_name(cfg->profile),
	       profile_read_limit(cfg->profile) < BASELINE_BACKGROUND_WORKERS,
	       profile_write_limit(cfg->profile) < BASELINE_BACKGROUND_WORKERS,
	       FOREGROUND_WORKERS, profile_read_limit(cfg->profile),
	       profile_write_limit(cfg->profile), BASELINE_BACKGROUND_WORKERS,
	       BASELINE_BACKGROUND_WORKERS, cfg->state_mode ? 1U : 0U,
	       cfg->warmup_ms, cfg->pre_ms, cfg->settle_ms,
	       cfg->measure_ms, cfg->foreground_uid, cfg->seed,
	       cfg->load_start_ns, cfg->pre_start_ns, cfg->action_apply_ns,
	       cfg->measure_start_ns, cfg->measure_end_ns, ret);
	for (uint32_t i = 0; i < created; i++) {
		printf("role=%s worker=%u uid=%u measured=%u pre=%u "
		       "treatment=%u total=%u "
		       "max_gate_wait_ns=%" PRIu64 " status=%d\n",
		       role_name(args[i].role), args[i].worker,
		       args[i].submit_uid, args[i].record_count,
		       args[i].pre_record_count,
		       args[i].treatment_record_count,
		       args[i].total_io_count, args[i].max_gate_wait_ns,
		       args[i].status);
		free(args[i].records);
	}
	if (cfg->state_mode)
		printf("pre_state=%s pre_fg_p99_ns=%" PRIu64
		       " pre_bg_read_p99_ns=%" PRIu64
		       " pre_bg_write_p99_ns=%" PRIu64
		       " read_state_threshold_ns=%" PRIu64
		       " write_state_threshold_ns=%" PRIu64 "\n",
		       pre_state_name(cfg, pre_bg_read_p99_ns,
				      pre_bg_write_p99_ns),
		       pre_fg_p99_ns, pre_bg_read_p99_ns,
		       pre_bg_write_p99_ns, cfg->read_state_threshold_ns,
		       cfg->write_state_threshold_ns);
	sem_destroy(&cfg->read_guard);
	sem_destroy(&cfg->write_guard);
	return ret;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"usage:\n"
		"  %s prepare --read-path FILE --write-path FILE --size-mb N\n"
		"  %s run --profile BASELINE|READ_GUARD|WRITE_GUARD|ALL_GUARD|"
		"READ_D2|WRITE_D2|ALL_D2 "
		"--read-path FILE --write-path FILE --csv FILE --file-mb N "
		"--warmup-ms N --measure-ms N --foreground-uid N --seed N\n"
		"  %s run-state --profile BASELINE|READ_GUARD|WRITE_GUARD|ALL_GUARD|"
		"READ_D2|WRITE_D2|ALL_D2 "
		"--read-path FILE --write-path FILE --csv FILE --file-mb N "
		"--warmup-ms N --pre-ms N --settle-ms N --measure-ms N "
		"--foreground-uid N --seed N [--read-state-threshold-ns N "
		"--write-state-threshold-ns N]\n",
		name, name, name);
}

static int parse_prepare(int argc, char **argv, const char **read_path,
			 const char **write_path, uint64_t *size_mb)
{
	static const struct option options[] = {
		{"read-path", required_argument, NULL, 'r'},
		{"write-path", required_argument, NULL, 'w'},
		{"size-mb", required_argument, NULL, 's'},
		{NULL, 0, NULL, 0},
	};
	int option;

	optind = 1;
	while ((option = getopt_long(argc, argv, "r:w:s:", options, NULL)) != -1) {
		switch (option) {
		case 'r': *read_path = optarg; break;
		case 'w': *write_path = optarg; break;
		case 's': if (parse_u64(optarg, size_mb)) return -EINVAL; break;
		default: return -EINVAL;
		}
	}
	return *read_path && *write_path && *size_mb ? 0 : -EINVAL;
}

static int parse_run(int argc, char **argv, struct run_config *cfg,
		     bool state_mode)
{
	static const struct option options[] = {
		{"profile", required_argument, NULL, 'p'},
		{"read-path", required_argument, NULL, 'r'},
		{"write-path", required_argument, NULL, 'w'},
		{"csv", required_argument, NULL, 'c'},
		{"file-mb", required_argument, NULL, 'f'},
		{"warmup-ms", required_argument, NULL, 'u'},
		{"pre-ms", required_argument, NULL, 'q'},
		{"settle-ms", required_argument, NULL, 't'},
		{"measure-ms", required_argument, NULL, 'm'},
		{"foreground-uid", required_argument, NULL, 'g'},
		{"seed", required_argument, NULL, 's'},
		{"read-state-threshold-ns", required_argument, NULL, 'x'},
		{"write-state-threshold-ns", required_argument, NULL, 'y'},
		{NULL, 0, NULL, 0},
	};
	uint64_t file_mb = 0;
	int option;
	bool profile_set = false;

	memset(cfg, 0, sizeof(*cfg));
	cfg->state_mode = state_mode;
	optind = 1;
	while ((option = getopt_long(argc, argv, "p:r:w:c:f:u:q:t:m:g:s:x:y:",
					     options, NULL)) != -1) {
		switch (option) {
		case 'p':
			if (parse_profile(optarg, &cfg->profile)) return -EINVAL;
			profile_set = true;
			break;
		case 'r': cfg->read_path = optarg; break;
		case 'w': cfg->write_path = optarg; break;
		case 'c': cfg->csv_path = optarg; break;
		case 'f': if (parse_u64(optarg, &file_mb)) return -EINVAL; break;
		case 'u': if (parse_u32(optarg, &cfg->warmup_ms)) return -EINVAL; break;
		case 'q': if (parse_u32(optarg, &cfg->pre_ms)) return -EINVAL; break;
		case 't': if (parse_u32(optarg, &cfg->settle_ms)) return -EINVAL; break;
		case 'm': if (parse_u32(optarg, &cfg->measure_ms)) return -EINVAL; break;
		case 'g': if (parse_u32(optarg, &cfg->foreground_uid)) return -EINVAL; break;
		case 's': if (parse_u64(optarg, &cfg->seed)) return -EINVAL; break;
		case 'x':
			if (parse_u64(optarg, &cfg->read_state_threshold_ns))
				return -EINVAL;
			break;
		case 'y':
			if (parse_u64(optarg, &cfg->write_state_threshold_ns))
				return -EINVAL;
			break;
		default: return -EINVAL;
		}
	}
	if (!profile_set || !cfg->read_path || !cfg->write_path ||
	    !cfg->csv_path || !file_mb || !cfg->warmup_ms ||
	    !cfg->measure_ms || !cfg->foreground_uid || !cfg->seed ||
	    (state_mode && (!cfg->pre_ms || !cfg->settle_ms)) ||
	    (!state_mode && (cfg->pre_ms || cfg->settle_ms)) ||
	    (!!cfg->read_state_threshold_ns !=
	     !!cfg->write_state_threshold_ns) ||
	    (profile_is_depth2(cfg->profile) &&
	     (!state_mode || !cfg->read_state_threshold_ns)))
		return -EINVAL;
	cfg->file_bytes = file_mb * 1024ULL * 1024ULL;
	return 0;
}

int main(int argc, char **argv)
{
	const char *read_path = NULL;
	const char *write_path = NULL;
	uint64_t size_mb = 0;
	struct run_config cfg;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "prepare")) {
		ret = parse_prepare(argc - 1, argv + 1, &read_path,
				    &write_path, &size_mb);
		if (!ret)
			ret = prepare_files(read_path, write_path, size_mb);
		if (!ret)
			printf("prepared read_path=%s write_path=%s size_mb=%" PRIu64
			       "\n", read_path, write_path, size_mb);
	} else if (!strcmp(argv[1], "run") ||
		   !strcmp(argv[1], "run-state")) {
		ret = parse_run(argc - 1, argv + 1, &cfg,
				!strcmp(argv[1], "run-state"));
		if (!ret)
			ret = run_trial(&cfg);
	} else {
		ret = -EINVAL;
	}
	if (ret)
		usage(argv[0]);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
