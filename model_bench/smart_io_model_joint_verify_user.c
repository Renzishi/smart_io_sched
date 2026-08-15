// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "smart_io_model_bench_uapi.h"

#define DEVICE_PATH "/dev/" SMART_IO_MODEL_BENCH_DEVICE_NAME
#define VECTOR_MAGIC 0x534a5631U
#define RESULT_MAGIC 0x534a5231U
#define VECTOR_VERSION 1U

struct vector_header {
	uint32_t magic;
	uint32_t version;
	uint32_t record_size;
	uint32_t count;
} __attribute__((packed));

struct vector_record {
	uint64_t row_id;
	uint32_t input_bits[SMART_IO_MODEL_INPUTS];
	uint32_t expected_action;
	uint32_t expected_q_bits[SMART_IO_MODEL_Q_VALUES];
} __attribute__((packed));

struct result_record {
	uint64_t row_id;
	uint32_t kernel_action;
	uint32_t kernel_q_bits[SMART_IO_MODEL_Q_VALUES];
} __attribute__((packed));

_Static_assert(sizeof(struct vector_header) == 16, "invalid vector header");
_Static_assert(sizeof(struct vector_record) == 104, "invalid vector record");
_Static_assert(sizeof(struct result_record) == 60, "invalid result record");

static int transfer_full(int fd, void *buffer, size_t length, int write_mode)
{
	uint8_t *cursor = buffer;

	while (length) {
		ssize_t ret = write_mode ? write(fd, cursor, length) : read(fd, cursor, length);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		cursor += ret;
		length -= (size_t)ret;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct vector_header input_header;
	struct vector_header output_header;
	struct vector_record input;
	struct result_record output;
	uint32_t action_mismatches = 0;
	uint32_t index;
	int input_fd;
	int output_fd;
	int device_fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s VECTOR_BIN RESULT_BIN\n", argv[0]);
		return EXIT_FAILURE;
	}
	input_fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	output_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	device_fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (input_fd < 0 || output_fd < 0 || device_fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}
	if (transfer_full(input_fd, &input_header, sizeof(input_header), 0) ||
	    input_header.magic != VECTOR_MAGIC ||
	    input_header.version != VECTOR_VERSION ||
	    input_header.record_size != sizeof(input)) {
		fprintf(stderr, "invalid vector header\n");
		return EXIT_FAILURE;
	}
	output_header.magic = RESULT_MAGIC;
	output_header.version = VECTOR_VERSION;
	output_header.record_size = sizeof(output);
	output_header.count = input_header.count;
	if (transfer_full(output_fd, &output_header, sizeof(output_header), 1)) {
		perror("write result header");
		return EXIT_FAILURE;
	}
	for (index = 0; index < input_header.count; index++) {
		struct smart_io_model_bench_sample sample;
		int ret;

		if (transfer_full(input_fd, &input, sizeof(input), 0)) {
			fprintf(stderr, "truncated vector at %u\n", index);
			return EXIT_FAILURE;
		}
		memset(&sample, 0, sizeof(sample));
		sample.version = SMART_IO_MODEL_BENCH_ABI_VERSION;
		sample.size = sizeof(sample);
		sample.sample_id = input.row_id;
		memcpy(sample.fp32_input_bits, input.input_bits,
		       sizeof(sample.fp32_input_bits));
		do {
			ret = ioctl(device_fd, SMART_IO_MODEL_BENCH_IOC_RUN_FP32, &sample);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0 || sample.action_id >= SMART_IO_MODEL_ACTIONS) {
			fprintf(stderr, "ioctl failed at %u: %s\n", index, strerror(errno));
			return EXIT_FAILURE;
		}
		output.row_id = input.row_id;
		output.kernel_action = sample.action_id;
		memcpy(output.kernel_q_bits, sample.fp32_q_bits,
		       sizeof(output.kernel_q_bits));
		if (output.kernel_action != input.expected_action)
			action_mismatches++;
		if (transfer_full(output_fd, &output, sizeof(output), 1)) {
			perror("write result");
			return EXIT_FAILURE;
		}
	}
	printf("samples=%u device_action_mismatches=%u\n", input_header.count,
	       action_mismatches);
	if (close(device_fd) || close(output_fd) || close(input_fd)) {
		perror("close");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
