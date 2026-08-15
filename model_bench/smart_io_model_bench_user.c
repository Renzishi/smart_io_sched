// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "smart_io_model_bench_uapi.h"
#define SMART_IO_MODEL_NOINLINE __attribute__((__noinline__))
#include "smart_io_model_int8.h"

#define DEFAULT_SAMPLE_COUNT 100000U
#define WARMUP_SAMPLE_COUNT 10000U
#define MAX_SAMPLE_COUNT 1000000U
#define DEVICE_PATH "/dev/" SMART_IO_MODEL_BENCH_DEVICE_NAME

enum benchmark_mode {
	BENCHMARK_FP32,
	BENCHMARK_INT8,
	BENCHMARK_KERNEL_INT8,
	BENCHMARK_KERNEL_FP32,
	BENCHMARK_KERNEL_EMPTY,
	BENCHMARK_KERNEL_NEON,
};

struct smart_io_model_fp32 {
	float weight1[SMART_IO_MODEL_HIDDEN1][SMART_IO_MODEL_INPUTS];
	float bias1[SMART_IO_MODEL_HIDDEN1];
	float weight2[SMART_IO_MODEL_HIDDEN2][SMART_IO_MODEL_HIDDEN1];
	float bias2[SMART_IO_MODEL_HIDDEN2];
	float level_weight[SMART_IO_MODEL_LEVEL_ACTIONS][SMART_IO_MODEL_HIDDEN2];
	float level_bias[SMART_IO_MODEL_LEVEL_ACTIONS];
	float policy_weight[SMART_IO_MODEL_POLICY_ACTIONS][SMART_IO_MODEL_HIDDEN2];
	float policy_bias[SMART_IO_MODEL_POLICY_ACTIONS];
};

struct sample_result {
	uint64_t duration_ns;
	uint32_t action_id;
	uint32_t level_action;
	uint32_t policy_action;
	uint32_t output_checksum;
	uint32_t fp32_q_bits[SMART_IO_MODEL_Q_VALUES];
};

/* First State row from 20260805_113818_window5_span10ms. */
static const uint32_t fixed_state_bits[SMART_IO_MODEL_INPUTS] = {
	0x3d042108, 0x00000000, 0x00000000, 0x3f87ef9e,
	0x3c06594b, 0x3f800000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000,
};

struct latency_stats {
	uint64_t min;
	uint64_t p50;
	uint64_t p90;
	uint64_t p99;
	uint64_t p999;
	uint64_t max;
	double mean;
};

static volatile uint32_t result_sink;

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void smart_io_model_fp32_init(struct smart_io_model_fp32 *fp32,
				     const struct smart_io_model_int8 *int8)
{
	int i;
	int output;

	for (output = 0; output < SMART_IO_MODEL_HIDDEN1; output++) {
		for (i = 0; i < SMART_IO_MODEL_INPUTS; i++)
			fp32->weight1[output][i] = int8->weight1[output][i] / 4.0f;
		fp32->bias1[output] = int8->bias1[output] / 32.0f;
	}
	for (output = 0; output < SMART_IO_MODEL_HIDDEN2; output++) {
		for (i = 0; i < SMART_IO_MODEL_HIDDEN1; i++)
			fp32->weight2[output][i] = int8->weight2[output][i] / 4.0f;
		fp32->bias2[output] = int8->bias2[output] / 32.0f;
	}
	for (output = 0; output < SMART_IO_MODEL_LEVEL_ACTIONS; output++) {
		for (i = 0; i < SMART_IO_MODEL_HIDDEN2; i++)
			fp32->level_weight[output][i] =
				int8->level_weight[output][i] / 4.0f;
		fp32->level_bias[output] = int8->level_bias[output] / 32.0f;
	}
	for (output = 0; output < SMART_IO_MODEL_POLICY_ACTIONS; output++) {
		for (i = 0; i < SMART_IO_MODEL_HIDDEN2; i++)
			fp32->policy_weight[output][i] =
				int8->policy_weight[output][i] / 4.0f;
		fp32->policy_bias[output] =
			int8->policy_bias[output] / 32.0f;
	}
}

static SMART_IO_MODEL_NOINLINE void
smart_io_model_fp32_infer(const struct smart_io_model_fp32 *model,
			  const float input[SMART_IO_MODEL_INPUTS],
			  uint32_t *level_action, uint32_t *policy_action,
			  uint32_t *output_checksum)
{
	float hidden1[SMART_IO_MODEL_HIDDEN1];
	float hidden2[SMART_IO_MODEL_HIDDEN2];
	float best_level_score = 0.0f;
	float best_policy_score = 0.0f;
	uint32_t best_level = 0;
	uint32_t best_policy = 0;
	uint32_t checksum = SMART_IO_MODEL_FNV_OFFSET;
	int i;
	int output;

	for (output = 0; output < SMART_IO_MODEL_HIDDEN1; output++) {
		float accumulator = model->bias1[output];

		for (i = 0; i < SMART_IO_MODEL_INPUTS; i++)
			accumulator += input[i] * model->weight1[output][i];
		hidden1[output] = accumulator > 0.0f ? accumulator : 0.0f;
	}
	for (output = 0; output < SMART_IO_MODEL_HIDDEN2; output++) {
		float accumulator = model->bias2[output];

		for (i = 0; i < SMART_IO_MODEL_HIDDEN1; i++)
			accumulator += hidden1[i] * model->weight2[output][i];
		hidden2[output] = accumulator > 0.0f ? accumulator : 0.0f;
	}
	for (output = 0; output < SMART_IO_MODEL_LEVEL_ACTIONS; output++) {
		float score = model->level_bias[output];
		int32_t quantized_score;

		for (i = 0; i < SMART_IO_MODEL_HIDDEN2; i++)
			score += hidden2[i] * model->level_weight[output][i];
		if (!output || score > best_level_score) {
			best_level_score = score;
			best_level = output;
		}
		quantized_score = (int32_t)(score * 16.0f);
		checksum = (checksum ^ (uint32_t)quantized_score) *
			SMART_IO_MODEL_FNV_PRIME;
	}
	for (output = 0; output < SMART_IO_MODEL_POLICY_ACTIONS; output++) {
		float score = model->policy_bias[output];
		int32_t quantized_score;

		for (i = 0; i < SMART_IO_MODEL_HIDDEN2; i++)
			score += hidden2[i] * model->policy_weight[output][i];
		if (!output || score > best_policy_score) {
			best_policy_score = score;
			best_policy = output;
		}
		quantized_score = (int32_t)(score * 16.0f);
		checksum = (checksum ^ (uint32_t)quantized_score) *
			SMART_IO_MODEL_FNV_PRIME;
	}
	*level_action = best_level;
	*policy_action = best_policy;
	*output_checksum = checksum;
}

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
	printf("%-20s min=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64
	       " p99=%" PRIu64 " p99.9=%" PRIu64 " max=%" PRIu64
	       " mean=%.2f ns\n",
	       name, stats->min, stats->p50, stats->p90, stats->p99,
	       stats->p999, stats->max, stats->mean);
}

static enum benchmark_mode parse_mode(const char *text)
{
	if (!strcmp(text, "fp32"))
		return BENCHMARK_FP32;
	if (!strcmp(text, "int8"))
		return BENCHMARK_INT8;
	if (!strcmp(text, "kernel-int8"))
		return BENCHMARK_KERNEL_INT8;
	if (!strcmp(text, "kernel-fp32"))
		return BENCHMARK_KERNEL_FP32;
	if (!strcmp(text, "kernel-empty"))
		return BENCHMARK_KERNEL_EMPTY;
	if (!strcmp(text, "kernel-neon"))
		return BENCHMARK_KERNEL_NEON;
	fprintf(stderr, "unknown mode: %s\n", text);
	exit(EXIT_FAILURE);
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

static unsigned int mode_ioctl(enum benchmark_mode mode)
{
	switch (mode) {
	case BENCHMARK_KERNEL_INT8:
		return SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_INT8;
	case BENCHMARK_KERNEL_FP32:
		return SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_FP32;
	case BENCHMARK_KERNEL_EMPTY:
		return SMART_IO_MODEL_BENCH_IOC_RUN_EMPTY;
	case BENCHMARK_KERNEL_NEON:
		return SMART_IO_MODEL_BENCH_IOC_RUN_NEON_CONTEXT;
	default:
		return 0;
	}
}

static int run_kernel_sample(int fd, enum benchmark_mode mode, uint64_t id,
			     struct sample_result *result)
{
	struct smart_io_model_bench_sample sample;
	int ret;

	memset(&sample, 0, sizeof(sample));
	sample.version = SMART_IO_MODEL_BENCH_ABI_VERSION;
	sample.size = sizeof(sample);
	sample.model_id = 0;
	if (mode == BENCHMARK_KERNEL_FP32) {
		memcpy(sample.fp32_input_bits, fixed_state_bits,
		       sizeof(fixed_state_bits));
	}
	do {
		ret = ioctl(fd, mode_ioctl(mode), &sample);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		fprintf(stderr, "ioctl failed at sample %" PRIu64 ": %s\n", id,
			strerror(errno));
		return -1;
	}
	result->duration_ns = sample.duration_ns;
	result->action_id = sample.action_id;
	result->level_action = sample.level_action;
	result->policy_action = sample.policy_action;
	result->output_checksum = sample.output_checksum;
	memcpy(result->fp32_q_bits, sample.fp32_q_bits,
	       sizeof(result->fp32_q_bits));
	return 0;
}

static int write_csv(const char *path, const struct sample_result *results,
		     size_t count)
{
	FILE *file = fopen(path, "w");
	size_t i;

	if (!file) {
		fprintf(stderr, "failed to open CSV %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	fputs("sample_id,duration_ns,action_id,level_action,policy_action,"
	      "output_checksum", file);
	for (i = 0; i < SMART_IO_MODEL_Q_VALUES; i++)
		fprintf(file, ",q%zu_bits", i);
	fputc('\n', file);
	for (i = 0; i < count; i++) {
		int q;

		fprintf(file, "%zu,%" PRIu64 ",%u,%u,%u,%u", i + 1,
			results[i].duration_ns, results[i].action_id,
			results[i].level_action, results[i].policy_action,
			results[i].output_checksum);
		for (q = 0; q < SMART_IO_MODEL_Q_VALUES; q++)
			fprintf(file, ",%u", results[i].fp32_q_bits[q]);
		fputc('\n', file);
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
	struct smart_io_model_int8 int8_model;
	struct smart_io_model_fp32 fp32_model;
	struct sample_result *results;
	uint64_t *durations;
	uint64_t *clock_cost;
	struct latency_stats duration_stats;
	struct latency_stats clock_stats;
	enum benchmark_mode mode;
	const char *csv_path = NULL;
	size_t sample_count = DEFAULT_SAMPLE_COUNT;
	uint32_t aggregate = SMART_IO_MODEL_FNV_OFFSET;
	size_t i;
	int fd = -1;
	int ret = EXIT_FAILURE;

	if (argc < 2 || argc > 4) {
		fprintf(stderr, "usage: %s MODE [sample_count] [csv_path]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	mode = parse_mode(argv[1]);
	if (argc > 2)
		sample_count = parse_sample_count(argv[2]);
	if (argc > 3)
		csv_path = argv[3];

	results = calloc(sample_count, sizeof(*results));
	durations = calloc(sample_count, sizeof(*durations));
	clock_cost = calloc(sample_count, sizeof(*clock_cost));
	if (!results || !durations || !clock_cost) {
		fprintf(stderr, "failed to allocate benchmark buffers\n");
		goto free_buffers;
	}
	smart_io_model_int8_init(&int8_model);
	smart_io_model_fp32_init(&fp32_model, &int8_model);

	if (mode >= BENCHMARK_KERNEL_INT8) {
		fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "failed to open %s: %s\n", DEVICE_PATH,
				strerror(errno));
			goto free_buffers;
		}
	}

	for (i = 0; i < WARMUP_SAMPLE_COUNT; i++) {
		__s8 input[SMART_IO_MODEL_INPUTS];
		uint32_t checksum;
		uint32_t level_action;
		uint32_t policy_action;

		smart_io_model_fill_input(UINT64_MAX - i, input);
		if (mode == BENCHMARK_FP32) {
			float fp_input[SMART_IO_MODEL_INPUTS];
			int j;

			for (j = 0; j < SMART_IO_MODEL_INPUTS; j++)
				fp_input[j] = input[j] / 64.0f;
			smart_io_model_fp32_infer(&fp32_model, fp_input,
						  &level_action,
						  &policy_action, &checksum);
		} else if (mode == BENCHMARK_INT8) {
			smart_io_model_int8_infer(&int8_model, input,
						  &level_action,
						  &policy_action, &checksum);
		} else if (run_kernel_sample(fd, mode, UINT64_MAX - i,
					     &results[0])) {
			goto close_device;
		} else {
			level_action = results[0].level_action;
			policy_action = results[0].policy_action;
			checksum = results[0].output_checksum;
		}
		result_sink ^= level_action ^ policy_action ^ checksum;
	}

	for (i = 0; i < sample_count; i++) {
		uint64_t start = now_ns();

		__asm__ __volatile__("" ::: "memory");
		clock_cost[i] = now_ns() - start;
	}
	for (i = 0; i < sample_count; i++) {
		__s8 input[SMART_IO_MODEL_INPUTS];
		uint32_t checksum;
		uint32_t level_action;
		uint32_t policy_action;
		uint64_t start;

		smart_io_model_fill_input(i + 1, input);
		if (mode == BENCHMARK_FP32) {
			float fp_input[SMART_IO_MODEL_INPUTS];
			int j;

			for (j = 0; j < SMART_IO_MODEL_INPUTS; j++)
				fp_input[j] = input[j] / 64.0f;
			start = now_ns();
			smart_io_model_fp32_infer(&fp32_model, fp_input,
						  &level_action,
						  &policy_action, &checksum);
			results[i].duration_ns = now_ns() - start;
			results[i].level_action = level_action;
			results[i].policy_action = policy_action;
			results[i].output_checksum = checksum;
		} else if (mode == BENCHMARK_INT8) {
			start = now_ns();
			smart_io_model_int8_infer(&int8_model, input,
						  &level_action,
						  &policy_action, &checksum);
			results[i].duration_ns = now_ns() - start;
			results[i].level_action = level_action;
			results[i].policy_action = policy_action;
			results[i].output_checksum = checksum;
		} else if (run_kernel_sample(fd, mode, i + 1, &results[i])) {
			goto close_device;
		}
		durations[i] = results[i].duration_ns;
		aggregate = (aggregate ^ results[i].level_action ^
			     results[i].policy_action ^
			     results[i].output_checksum) * SMART_IO_MODEL_FNV_PRIME;
		result_sink ^= aggregate;
	}

	duration_stats = calculate_stats(durations, sample_count);
	clock_stats = calculate_stats(clock_cost, sample_count);
	printf("mode=%s samples=%zu warmup=%u aggregate=%08x\n", argv[1],
	       sample_count, WARMUP_SAMPLE_COUNT, aggregate);
	print_stats("duration", &duration_stats);
	print_stats("user_clock_pair", &clock_stats);
	if (csv_path && write_csv(csv_path, results, sample_count))
		goto close_device;
	ret = EXIT_SUCCESS;

close_device:
	if (fd >= 0 && close(fd))
		fprintf(stderr, "close failed: %s\n", strerror(errno));
free_buffers:
	free(results);
	free(durations);
	free(clock_cost);
	return ret;
}
