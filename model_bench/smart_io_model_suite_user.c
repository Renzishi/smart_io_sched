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

#include "smart_io_model_bench_uapi.h"

#define DEVICE_PATH "/dev/" SMART_IO_MODEL_BENCH_DEVICE_NAME
#define WARMUP_SAMPLES 10000U
#define VECTOR_MAGIC 0x53515631U
#define RESULT_MAGIC 0x53525231U

struct vector_header {
	uint32_t magic;
	uint32_t version;
	uint32_t model_id;
	uint32_t count;
};

struct vector_record {
	uint32_t input_bits[SMART_IO_MODEL_INPUTS];
	int8_t int8_input[SMART_IO_MODEL_INPUTS];
	uint32_t fp32_action;
	uint32_t int8_action;
};

struct result_header {
	uint32_t magic;
	uint32_t version;
	uint32_t model_id;
	uint32_t count;
};

struct result_record {
	uint32_t action;
	uint32_t q_bits[SMART_IO_MODEL_Q_VALUES];
	int32_t int8_q[SMART_IO_MODEL_Q_VALUES];
};

static const uint32_t fixed_state_bits[SMART_IO_MODEL_INPUTS] = {
	0x3d042108, 0x00000000, 0x00000000, 0x3f87ef9e,
	0x3c06594b, 0x3f800000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000,
};
static const int8_t fixed_int8_input[SMART_IO_MODEL_INPUTS] = {
	-19, -14, -28, -4, -12, 6, -15, -7, -5, -1, -3,
};

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static unsigned long parse_value(const char *text, unsigned long maximum)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || *end || value > maximum) {
		fprintf(stderr, "invalid number: %s\n", text);
		exit(EXIT_FAILURE);
	}
	return value;
}

static unsigned int mode_ioctl(const char *mode)
{
	if (!strcmp(mode, "fp32"))
		return SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_FP32;
	if (!strcmp(mode, "int8"))
		return SMART_IO_MODEL_BENCH_IOC_RUN_SUITE_INT8;
	fprintf(stderr, "unknown mode: %s\n", mode);
	exit(EXIT_FAILURE);
}

static int run_sample(int fd, unsigned int command, uint32_t model_id,
		      const uint32_t input_bits[SMART_IO_MODEL_INPUTS],
		      const int8_t int8_input[SMART_IO_MODEL_INPUTS],
		      struct smart_io_model_bench_sample *sample)
{
	int ret;

	memset(sample, 0, sizeof(*sample));
	sample->version = SMART_IO_MODEL_BENCH_ABI_VERSION;
	sample->size = sizeof(*sample);
	sample->model_id = model_id;
	memcpy(sample->fp32_input_bits, input_bits,
	       sizeof(sample->fp32_input_bits));
	memcpy(sample->int8_input, int8_input, sizeof(sample->int8_input));
	do {
		ret = ioctl(fd, command, sample);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int benchmark(const char *mode, uint32_t model_id, size_t count,
		     const char *csv_path)
{
	unsigned int command = mode_ioctl(mode);
	struct smart_io_model_bench_sample sample;
	uint64_t *durations;
	uint64_t sum = 0;
	uint32_t aggregate = 2166136261U;
	FILE *csv = NULL;
	size_t i;
	int fd;
	int ret = EXIT_FAILURE;

	fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open benchmark device");
		return EXIT_FAILURE;
	}
	durations = calloc(count, sizeof(*durations));
	if (!durations) {
		perror("calloc");
		goto close_device;
	}
	if (csv_path) {
		csv = fopen(csv_path, "w");
		if (!csv) {
			perror("open CSV");
			goto free_durations;
		}
		fputs("sample_id,duration_ns,action_id,output_checksum\n", csv);
	}
	for (i = 0; i < WARMUP_SAMPLES; i++) {
		if (run_sample(fd, command, model_id, fixed_state_bits,
			       fixed_int8_input, &sample))
			goto close_csv;
	}
	for (i = 0; i < count; i++) {
		if (run_sample(fd, command, model_id, fixed_state_bits,
			       fixed_int8_input, &sample))
			goto close_csv;
		durations[i] = sample.duration_ns;
		sum += sample.duration_ns;
		aggregate = (aggregate ^ sample.action_id ^ sample.output_checksum) *
			16777619U;
		if (csv)
			fprintf(csv, "%zu,%llu,%u,%u\n", i + 1,
				(unsigned long long)sample.duration_ns, sample.action_id,
				sample.output_checksum);
	}
	qsort(durations, count, sizeof(*durations), compare_u64);
	printf("mode=%s model_id=%u samples=%zu warmup=%u aggregate=%08x\n",
	       mode, model_id, count, WARMUP_SAMPLES, aggregate);
	printf("duration min=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64
	       " p99=%" PRIu64 " p99.9=%" PRIu64 " max=%" PRIu64
	       " mean=%.2f ns\n",
	       durations[0], durations[(count - 1) * 50 / 100],
	       durations[(count - 1) * 90 / 100],
	       durations[(count - 1) * 99 / 100],
	       durations[(count - 1) * 999 / 1000], durations[count - 1],
	       (double)sum / count);
	ret = EXIT_SUCCESS;

close_csv:
	if (csv)
		fclose(csv);
free_durations:
	free(durations);
close_device:
	close(fd);
	return ret;
}

static int verify(const char *mode, const char *vector_path,
		  const char *result_path)
{
	unsigned int command = mode_ioctl(mode);
	struct vector_header input_header;
	struct result_header output_header;
	struct vector_record input;
	struct result_record output;
	struct smart_io_model_bench_sample sample;
	FILE *vectors;
	FILE *results;
	uint32_t index;
	int fd;
	int ret = EXIT_FAILURE;

	vectors = fopen(vector_path, "rb");
	if (!vectors) {
		perror("open vectors");
		return EXIT_FAILURE;
	}
	if (fread(&input_header, sizeof(input_header), 1, vectors) != 1 ||
	    input_header.magic != VECTOR_MAGIC || input_header.version != 1U) {
		fprintf(stderr, "invalid vector header\n");
		goto close_vectors;
	}
	fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open benchmark device");
		goto close_vectors;
	}
	results = fopen(result_path, "wb");
	if (!results) {
		perror("open results");
		goto close_device;
	}
	output_header.magic = RESULT_MAGIC;
	output_header.version = 1U;
	output_header.model_id = input_header.model_id;
	output_header.count = input_header.count;
	if (fwrite(&output_header, sizeof(output_header), 1, results) != 1) {
		perror("write result header");
		goto close_results;
	}
	for (index = 0; index < input_header.count; index++) {
		if (fread(&input, sizeof(input), 1, vectors) != 1 ||
		    run_sample(fd, command, input_header.model_id,
			       input.input_bits, input.int8_input, &sample)) {
			fprintf(stderr, "verification failed at %u\n", index);
			goto close_results;
		}
		output.action = sample.action_id;
		memcpy(output.q_bits, sample.fp32_q_bits, sizeof(output.q_bits));
		memcpy(output.int8_q, sample.int8_q, sizeof(output.int8_q));
		if (fwrite(&output, sizeof(output), 1, results) != 1) {
			perror("write result");
			goto close_results;
		}
	}
	printf("verified mode=%s model_id=%u samples=%u\n", mode,
	       input_header.model_id, input_header.count);
	ret = EXIT_SUCCESS;

close_results:
	fclose(results);
close_device:
	close(fd);
close_vectors:
	fclose(vectors);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "verify"))
		return verify(argv[2], argv[3], argv[4]);
	if (argc < 4 || argc > 5) {
		fprintf(stderr, "usage: %s <fp32|int8> <model_id> <samples> [csv]\n"
			"       %s verify <fp32|int8> <vectors> <results>\n",
			argv[0], argv[0]);
		return EXIT_FAILURE;
	}
	return benchmark(argv[1], parse_value(argv[2], 7U),
			 parse_value(argv[3], 1000000U),
			 argc == 5 ? argv[4] : NULL);
}
