#ifndef RUST_ALLOC_H
#define RUST_ALLOC_H

#include <misc/types.h>
#include <utilities/zero_copy.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t size;
	uint32_t used;
	uint32_t next;
	uint32_t magic;
} rust_heap_block_header_t;

typedef enum {
	RUST_HEAP_VALIDATION_OK = 0,
	RUST_HEAP_VALIDATION_NULL_BLOCK = 1,
	RUST_HEAP_VALIDATION_BAD_HEAP_BASE = 2,
	RUST_HEAP_VALIDATION_HEAP_OVERFLOW = 3,
	RUST_HEAP_VALIDATION_BLOCK_OUTSIDE_HEAP = 4,
	RUST_HEAP_VALIDATION_OFFSET_MISMATCH = 5,
	RUST_HEAP_VALIDATION_BLOCK_RANGE_INVALID = 6,
	RUST_HEAP_VALIDATION_BLOCK_SIZE_INVALID = 7
} rust_heap_validation_result_t;

zero_copy_buffer_t* rust_zero_copy_buffer_create(uint32_t size);
void rust_zero_copy_buffer_destroy(zero_copy_buffer_t* buffer);
int rust_zero_copy_buffer_read(const zero_copy_buffer_t* buffer, void* data, uint32_t offset, uint32_t size);
int rust_zero_copy_buffer_write(zero_copy_buffer_t* buffer, const void* data, uint32_t offset, uint32_t size);

rust_heap_validation_result_t rust_validate_heap_block(
	const rust_heap_block_header_t* block,
	uint32_t offset,
	const uint8_t* heap_start,
	uint32_t heap_size,
	uint32_t heap_size_min,
	uint32_t block_header_size);

#ifdef __cplusplus
}
#endif

#endif
