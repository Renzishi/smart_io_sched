// SPDX-License-Identifier: Apache-2.0
/* Bounded Android AArch64 direct-I/O profile generator. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
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

#ifndef __NR_ioprio_set
#if defined(__aarch64__)
#define __NR_ioprio_set 30
#else
#error "This test is intended for Android AArch64"
#endif
#endif

#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_NONE 0
#define IOPRIO_CLASS_RT 1
#define IOPRIO_CLASS_BE 2
#define IOPRIO_CLASS_IDLE 3
#define IOPRIO_PRIO_VALUE(class, data) (((class) << 13) | (data))
#define ALIGNMENT 4096U
#define MAX_WORKERS 32U
#define MAX_BLOCK_SIZE (1024U * 1024U)

enum profile_op {
	PROFILE_READ,
	PROFILE_WRITE,
};

struct probe_config {
	const char *path;
	const char *csv_path;
	enum profile_op op;
	uint64_t file_bytes;
	uint32_t block_bytes;
	uint32_t samples;
	uint32_t workers;
	uint32_t ioprio_class;
	uint32_t ioprio_data;
	uint64_t seed;
};

struct worker_arg {
	const struct probe_config *cfg;
	FILE *csv;
	pthread_mutex_t *csv_lock;
	uint32_t worker;
	uint32_t samples;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

static int set_ioprio(uint32_t class, uint32_t data)
{
	int value;

	if (class > IOPRIO_CLASS_IDLE || data > 7)
		return -EINVAL;
	value = (int)syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0,
				     IOPRIO_PRIO_VALUE(class, data));
	if (value < 0)
		return -errno;
	return 0;
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

static int parse_ioprio(const char *text, uint32_t *class,
			       uint32_t *data)
{
	if (!strcmp(text, "none"))
		*class = IOPRIO_CLASS_NONE;
	else if (!strcmp(text, "rt"))
		*class = IOPRIO_CLASS_RT;
	else if (!strcmp(text, "be"))
		*class = IOPRIO_CLASS_BE;
	else if (!strcmp(text, "idle"))
		*class = IOPRIO_CLASS_IDLE;
	else
		return -EINVAL;
	*data = 0;
	return 0;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"usage:\n"
		"  %s prepare --path FILE --size-mb N\n"
		"  %s probe --path FILE --csv FILE --op read|write "
		"--samples N --workers N --block-kb N --ioprio none|rt|be|idle "
		"[--seed N]\n",
		name, name);
}

static int open_direct(const char *path, int flags)
{
	int fd = open(path, flags | O_CLOEXEC | O_DIRECT, 0600);

	if (fd < 0)
		fprintf(stderr, "open %s failed: errno=%d (%s)\n", path, errno,
			strerror(errno));
	return fd;
}

static int prepare_file(const char *path, uint64_t size_mb)
{
	const uint32_t block_bytes = 1024U * 1024U;
	uint64_t total_bytes = size_mb * 1024ULL * 1024ULL;
	uint64_t offset;
	unsigned char *buffer = NULL;
	int fd = -1;
	int ret = -1;

	if (!size_mb || total_bytes % block_bytes)
		return -EINVAL;
	if (posix_memalign((void **)&buffer, ALIGNMENT, block_bytes))
		return -ENOMEM;
	for (uint32_t i = 0; i < block_bytes; i++)
		buffer[i] = (unsigned char)(i * 131U + 17U);
	fd = open_direct(path, O_CREAT | O_RDWR | O_TRUNC | O_SYNC);
	if (fd < 0)
		goto out;
	if (ftruncate(fd, (off_t)total_bytes) != 0) {
		fprintf(stderr, "ftruncate failed: errno=%d (%s)\n", errno,
			strerror(errno));
		goto out;
	}
	for (offset = 0; offset < total_bytes; offset += block_bytes) {
		ssize_t written = pwrite(fd, buffer, block_bytes, (off_t)offset);

		if (written != (ssize_t)block_bytes) {
			fprintf(stderr, "prepare pwrite offset=%" PRIu64
				" ret=%zd errno=%d (%s)\n", offset, written, errno,
				strerror(errno));
			goto out;
		}
	}
	if (fsync(fd) != 0) {
		fprintf(stderr, "prepare fsync failed: errno=%d (%s)\n", errno,
			strerror(errno));
		goto out;
	}
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return ret;
}

static void *probe_worker(void *opaque)
{
	struct worker_arg *arg = opaque;
	const struct probe_config *cfg = arg->cfg;
	unsigned char *buffer = NULL;
	uint64_t state = cfg->seed ^ ((uint64_t)arg->worker + 1U) *
				  0xd6e8feb86659fd93ULL;
	uint64_t span = cfg->file_bytes - cfg->block_bytes;
	uint32_t i;
	int fd;
	int priority_ret;

	fd = open_direct(cfg->path, O_RDWR | (cfg->op == PROFILE_WRITE ? O_SYNC : 0));
	if (fd < 0)
		return (void *)(intptr_t)-1;
	priority_ret = set_ioprio(cfg->ioprio_class, cfg->ioprio_data);
	if (priority_ret)
		fprintf(stderr, "worker=%u ioprio failed: %d\n", arg->worker,
			priority_ret);
	if (posix_memalign((void **)&buffer, ALIGNMENT, cfg->block_bytes)) {
		close(fd);
		return (void *)(intptr_t)-1;
	}
	memset(buffer, (int)(arg->worker + 31U), cfg->block_bytes);
	for (i = 0; i < arg->samples; i++) {
		uint64_t slot = next_random(&state) % (span / cfg->block_bytes + 1U);
		uint64_t offset = slot * cfg->block_bytes;
		uint64_t start = monotonic_ns();
		uint64_t end;
		ssize_t io_ret;
		int saved_errno = 0;

		if (cfg->op == PROFILE_READ)
			io_ret = pread(fd, buffer, cfg->block_bytes, (off_t)offset);
		else
			io_ret = pwrite(fd, buffer, cfg->block_bytes, (off_t)offset);
		end = monotonic_ns();
		if (io_ret < 0)
			saved_errno = errno;
		pthread_mutex_lock(arg->csv_lock);
		fprintf(arg->csv,
			"%u,%u,%s,%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64
			",%" PRIu64 ",%zd,%d,%u,%u\n",
			arg->worker, i, cfg->op == PROFILE_READ ? "read" : "write",
			offset, cfg->block_bytes, start, end, end - start, io_ret,
			saved_errno, cfg->ioprio_class, cfg->ioprio_data);
		pthread_mutex_unlock(arg->csv_lock);
	}
	free(buffer);
	close(fd);
	return NULL;
}

static int run_probe(const struct probe_config *cfg)
{
	pthread_t threads[MAX_WORKERS];
	struct worker_arg args[MAX_WORKERS];
	pthread_mutex_t csv_lock = PTHREAD_MUTEX_INITIALIZER;
	FILE *csv;
	uint32_t i;
	int ret = -1;

	if (cfg->workers == 0 || cfg->workers > MAX_WORKERS ||
		cfg->samples < cfg->workers || !cfg->file_bytes ||
		cfg->block_bytes < ALIGNMENT || cfg->block_bytes > MAX_BLOCK_SIZE ||
		(cfg->block_bytes & (ALIGNMENT - 1)) ||
		cfg->file_bytes < cfg->block_bytes)
		return -EINVAL;
	csv = fopen(cfg->csv_path, "w");
	if (!csv) {
		fprintf(stderr, "open CSV %s failed: errno=%d (%s)\n",
			cfg->csv_path, errno, strerror(errno));
		return -errno;
	}
	fputs("worker,sample,op,offset_bytes,size_bytes,start_ns,end_ns,"
	      "latency_ns,ret,errno,ioprio_class,ioprio_data\n", csv);
	for (i = 0; i < cfg->workers; i++) {
		args[i].cfg = cfg;
		args[i].csv = csv;
		args[i].csv_lock = &csv_lock;
		args[i].worker = i;
		args[i].samples = (cfg->samples + i) / cfg->workers;
		if (pthread_create(&threads[i], NULL, probe_worker, &args[i])) {
			fprintf(stderr, "pthread_create failed for worker=%u\n", i);
			goto join;
		}
	}
	ret = 0;
join:
	for (uint32_t j = 0; j < i; j++)
		pthread_join(threads[j], NULL);
	fflush(csv);
	fclose(csv);
	return ret;
}

static int parse_probe(int argc, char **argv, struct probe_config *cfg)
{
	static const struct option options[] = {
		{"path", required_argument, NULL, 'p'},
		{"csv", required_argument, NULL, 'c'},
		{"op", required_argument, NULL, 'o'},
		{"samples", required_argument, NULL, 'n'},
		{"workers", required_argument, NULL, 'w'},
		{"block-kb", required_argument, NULL, 'b'},
		{"file-mb", required_argument, NULL, 'f'},
		{"ioprio", required_argument, NULL, 'i'},
		{"seed", required_argument, NULL, 's'},
		{NULL, 0, NULL, 0},
	};
	int option;

	memset(cfg, 0, sizeof(*cfg));
	cfg->op = PROFILE_READ;
	cfg->workers = 1;
	cfg->block_bytes = 4096;
	cfg->samples = 100;
	cfg->file_bytes = 64ULL * 1024ULL * 1024ULL;
	cfg->seed = 1;
	optind = 1;
	while ((option = getopt_long(argc, argv, "p:c:o:n:w:b:f:i:s:",
					     options, NULL)) != -1) {
		switch (option) {
		case 'p': cfg->path = optarg; break;
		case 'c': cfg->csv_path = optarg; break;
		case 'o':
			if (!strcmp(optarg, "read")) cfg->op = PROFILE_READ;
			else if (!strcmp(optarg, "write")) cfg->op = PROFILE_WRITE;
			else return -EINVAL;
			break;
		case 'n': if (parse_u32(optarg, &cfg->samples)) return -EINVAL; break;
		case 'w': if (parse_u32(optarg, &cfg->workers)) return -EINVAL; break;
		case 'b':
			if (parse_u32(optarg, &cfg->block_bytes)) return -EINVAL;
			cfg->block_bytes *= 1024U;
			break;
		case 'f':
			if (parse_u64(optarg, &cfg->file_bytes)) return -EINVAL;
			cfg->file_bytes *= 1024ULL * 1024ULL;
			break;
		case 'i': if (parse_ioprio(optarg, &cfg->ioprio_class, &cfg->ioprio_data)) return -EINVAL; break;
		case 's': if (parse_u64(optarg, &cfg->seed)) return -EINVAL; break;
		default: return -EINVAL;
		}
	}
	if (!cfg->path || !cfg->csv_path)
		return -EINVAL;
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t size_mb;
	struct probe_config cfg;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "prepare")) {
		if (argc != 6 || strcmp(argv[2], "--path") ||
			strcmp(argv[4], "--size-mb") || parse_u64(argv[5], &size_mb)) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		ret = prepare_file(argv[3], size_mb);
		if (!ret)
			printf("prepared path=%s size_mb=%" PRIu64 "\n", argv[3], size_mb);
		return ret ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (strcmp(argv[1], "probe")) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	ret = parse_probe(argc - 1, argv + 1, &cfg);
	if (ret) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	ret = run_probe(&cfg);
	printf("probe op=%s samples=%u workers=%u block_bytes=%u ioprio_class=%u csv=%s ret=%d\n",
		cfg.op == PROFILE_READ ? "read" : "write", cfg.samples,
		cfg.workers, cfg.block_bytes, cfg.ioprio_class, cfg.csv_path, ret);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
