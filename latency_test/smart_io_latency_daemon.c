// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "smart_io_latency_uapi.h"

#define DEFAULT_SAMPLE_COUNT 10000U
#define WARMUP_SAMPLE_COUNT 1000U
#define MAX_SAMPLE_COUNT 1000000U
#define DEVICE_PATH "/dev/" SMART_IO_LATENCY_DEVICE_NAME

struct latency_stats {
	uint64_t min;
	uint64_t p50;
	uint64_t p90;
	uint64_t p99;
	uint64_t p999;
	uint64_t max;
	double mean;
};

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static struct latency_stats calculate_stats(uint64_t *values, size_t count)
{
	struct latency_stats stats;
	long double sum = 0;
	size_t i;

	qsort(values, count, sizeof(*values), compare_u64);
	for (i = 0; i < count; i++)
		sum += values[i];
	stats.min = values[0];
	stats.p50 = values[(count - 1) * 50 / 100];
	stats.p90 = values[(count - 1) * 90 / 100];
	stats.p99 = values[(count - 1) * 99 / 100];
	stats.p999 = values[(count - 1) * 999 / 1000];
	stats.max = values[count - 1];
	stats.mean = (double)(sum / count);
	return stats;
}

static void print_stats(const char *name, const struct latency_stats *stats)
{
	printf("%-22s min=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64
	       " p99=%" PRIu64 " p99.9=%" PRIu64 " max=%" PRIu64
	       " mean=%.2f ns\n",
	       name, stats->min, stats->p50, stats->p90, stats->p99,
	       stats->p999, stats->max, stats->mean);
}

static size_t parse_sample_count(const char *text)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || *end != '\0' || !value ||
	    value > MAX_SAMPLE_COUNT) {
		fprintf(stderr, "invalid sample count: %s\n", text);
		exit(EXIT_FAILURE);
	}
	return value;
}

static int run_sample(int fd, struct smart_io_latency_result *result)
{
	struct smart_io_latency_request request;
	struct smart_io_latency_action action;
	ssize_t ret;

	do {
		ret = read(fd, &request, sizeof(request));
	} while (ret < 0 && errno == EINTR);
	if (ret != (ssize_t)sizeof(request)) {
		fprintf(stderr, "read failed: ret=%zd errno=%d (%s)\n", ret,
			errno, strerror(errno));
		return -1;
	}
	if (request.version != SMART_IO_LATENCY_ABI_VERSION ||
	    request.size != sizeof(request)) {
		fprintf(stderr, "request ABI mismatch\n");
		return -1;
	}

	memset(&action, 0, sizeof(action));
	action.version = SMART_IO_LATENCY_ABI_VERSION;
	action.size = sizeof(action);
	action.sequence_id = request.sequence_id;
	action.throttle_level = SMART_IO_LATENCY_THROTTLE_MEDIUM;
	action.dispatch_policy = SMART_IO_LATENCY_DISPATCH_BASELINE;
	do {
		ret = write(fd, &action, sizeof(action));
	} while (ret < 0 && errno == EINTR);
	if (ret != (ssize_t)sizeof(action)) {
		fprintf(stderr, "write failed: ret=%zd errno=%d (%s)\n", ret,
			errno, strerror(errno));
		return -1;
	}

	do {
		ret = ioctl(fd, SMART_IO_LATENCY_IOC_GET_LAST, result);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		fprintf(stderr, "GET_LAST failed: errno=%d (%s)\n", errno,
			strerror(errno));
		return -1;
	}
	if (result->version != SMART_IO_LATENCY_ABI_VERSION ||
	    result->size != sizeof(*result) ||
	    result->sequence_id != request.sequence_id) {
		fprintf(stderr, "result ABI or sequence mismatch\n");
		return -1;
	}
	return 0;
}

static int write_csv(const char *path,
		     const struct smart_io_latency_result *results, size_t count)
{
	FILE *file;
	size_t i;

	file = fopen(path, "w");
	if (!file) {
		fprintf(stderr, "failed to open CSV %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	fputs("sequence_id,wake_ts_ns,write_enter_ts_ns,decision_end_ts_ns,"
	      "wake_to_write_enter_ns,kernel_apply_ns,total_ns,trigger_cpu,"
	      "write_cpu,waiter_present,status\n",
	      file);
	for (i = 0; i < count; i++) {
		const struct smart_io_latency_result *result = &results[i];

		fprintf(file,
			"%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
			",%" PRIu64 ",%" PRIu64 ",%" PRIu64
			",%u,%u,%u,%u\n",
			(uint64_t)result->sequence_id,
			(uint64_t)result->wake_ts_ns,
			(uint64_t)result->write_enter_ts_ns,
			(uint64_t)result->decision_end_ts_ns,
			(uint64_t)result->wake_to_write_enter_ns,
			(uint64_t)result->kernel_apply_ns,
			(uint64_t)result->total_ns, result->trigger_cpu,
			result->write_cpu, result->waiter_present,
			result->status);
	}
	if (fclose(file)) {
		fprintf(stderr, "failed to close CSV %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	size_t sample_count = DEFAULT_SAMPLE_COUNT;
	const char *csv_path = NULL;
	struct smart_io_latency_result *results;
	struct smart_io_latency_result warmup_result;
	uint64_t *wake_to_write;
	uint64_t *kernel_apply;
	uint64_t *total;
	struct latency_stats wake_stats;
	struct latency_stats apply_stats;
	struct latency_stats total_stats;
	size_t valid_count = 0;
	size_t applied_count = 0;
	size_t late_count = 0;
	size_t no_waiter_count = 0;
	size_t invalid_count = 0;
	size_t over_1ms_count = 0;
	size_t over_2ms_count = 0;
	size_t over_3ms_count = 0;
	size_t i;
	int fd;
	int ret = EXIT_FAILURE;

	if (argc > 3) {
		fprintf(stderr, "usage: %s [sample_count] [csv_path]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc > 1)
		sample_count = parse_sample_count(argv[1]);
	if (argc > 2)
		csv_path = argv[2];

	results = calloc(sample_count, sizeof(*results));
	wake_to_write = calloc(sample_count, sizeof(*wake_to_write));
	kernel_apply = calloc(sample_count, sizeof(*kernel_apply));
	total = calloc(sample_count, sizeof(*total));
	if (!results || !wake_to_write || !kernel_apply || !total) {
		fprintf(stderr, "failed to allocate result buffers\n");
		goto free_buffers;
	}

	fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s: %s\n", DEVICE_PATH,
			strerror(errno));
		goto free_buffers;
	}
	for (i = 0; i < WARMUP_SAMPLE_COUNT; i++) {
		if (run_sample(fd, &warmup_result))
			goto close_device;
	}
	for (i = 0; i < sample_count; i++) {
		if (run_sample(fd, &results[i]))
			goto close_device;
	}

	for (i = 0; i < sample_count; i++) {
		const struct smart_io_latency_result *result = &results[i];

		switch (result->status) {
		case SMART_IO_LATENCY_STATUS_APPLIED:
			applied_count++;
			break;
		case SMART_IO_LATENCY_STATUS_LATE:
			late_count++;
			break;
		case SMART_IO_LATENCY_STATUS_NO_WAITER:
			no_waiter_count++;
			continue;
		default:
			invalid_count++;
			continue;
		}
		wake_to_write[valid_count] = result->wake_to_write_enter_ns;
		kernel_apply[valid_count] = result->kernel_apply_ns;
		total[valid_count] = result->total_ns;
		if (result->total_ns > 1000000ULL)
			over_1ms_count++;
		if (result->total_ns > 2000000ULL)
			over_2ms_count++;
		if (result->total_ns > SMART_IO_LATENCY_DEADLINE_NS)
			over_3ms_count++;
		valid_count++;
	}
	if (!valid_count) {
		fprintf(stderr, "no valid samples collected\n");
		goto close_device;
	}

	wake_stats = calculate_stats(wake_to_write, valid_count);
	apply_stats = calculate_stats(kernel_apply, valid_count);
	total_stats = calculate_stats(total, valid_count);
	printf("device=%s warmup=%u requested=%zu valid=%zu\n", DEVICE_PATH,
	       WARMUP_SAMPLE_COUNT, sample_count, valid_count);
	printf("applied=%zu late=%zu no_waiter=%zu invalid=%zu "
	       "timeout_rate=%.6f%%\n",
	       applied_count, late_count, no_waiter_count, invalid_count,
	       100.0 * late_count / valid_count);
	printf("over_1ms=%zu (%.6f%%) over_2ms=%zu (%.6f%%) "
	       "over_3ms=%zu (%.6f%%)\n",
	       over_1ms_count, 100.0 * over_1ms_count / valid_count,
	       over_2ms_count, 100.0 * over_2ms_count / valid_count,
	       over_3ms_count, 100.0 * over_3ms_count / valid_count);
	print_stats("wake_to_write_enter", &wake_stats);
	print_stats("kernel_apply", &apply_stats);
	print_stats("total", &total_stats);
	if (csv_path && write_csv(csv_path, results, sample_count))
		goto close_device;
	ret = EXIT_SUCCESS;

close_device:
	if (close(fd))
		fprintf(stderr, "close failed: %s\n", strerror(errno));
free_buffers:
	free(results);
	free(wake_to_write);
	free(kernel_apply);
	free(total);
	return ret;
}
