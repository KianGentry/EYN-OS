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

typedef enum {
	RUST_HEAP_MATH_OK = 0,
	RUST_HEAP_MATH_INVALID_ARG = 1,
	RUST_HEAP_MATH_OVERFLOW = 2,
	RUST_HEAP_MATH_NO_SPLIT = 3
} rust_heap_math_result_t;

typedef enum {
	RUST_HEAP_COALESCE_SKIP = 0,
	RUST_HEAP_COALESCE_MERGE = 1,
	RUST_HEAP_COALESCE_OVERFLOW = 2
} rust_heap_coalesce_result_t;

typedef enum {
	RUST_HEAP_PTR_OK = 0,
	RUST_HEAP_PTR_INVALID_ARG = 1,
	RUST_HEAP_PTR_UNDERFLOW = 2,
	RUST_HEAP_PTR_OUT_OF_RANGE = 3,
	RUST_HEAP_PTR_OVERFLOW = 4
} rust_heap_ptr_result_t;

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

rust_heap_math_result_t rust_heap_compute_total_size(
	uint32_t nbytes,
	uint32_t block_header_size,
	uint32_t align,
	uint32_t* out_total_size);

rust_heap_math_result_t rust_heap_plan_split(
	uint32_t block_offset,
	uint32_t block_size,
	uint32_t needed_size,
	uint32_t block_header_size,
	uint32_t min_block_size,
	uint32_t* out_new_block_offset,
	uint32_t* out_new_block_size);

rust_heap_coalesce_result_t rust_heap_plan_coalesce(
	uint32_t block_used,
	uint32_t next_used,
	uint32_t block_size,
	uint32_t next_size,
	uint32_t* out_merged_size);

rust_heap_math_result_t rust_heap_compute_max_allocation(
	uint32_t heap_size,
	uint32_t numerator,
	uint32_t denominator,
	uint32_t* out_max_allocation);

rust_heap_ptr_result_t rust_heap_compute_block_offset(
	uintptr ptr_addr,
	uintptr heap_start_addr,
	uint32_t block_header_size,
	uint32_t heap_size,
	uint32_t* out_block_offset);

typedef enum {
	RUST_HEAP_BEST_FIT_SKIP = 0,
	RUST_HEAP_BEST_FIT_UPDATE = 1
} rust_heap_best_fit_result_t;

rust_heap_best_fit_result_t rust_heap_best_fit_update(
	uint32_t block_size,
	uint32_t current_best_size,
	uint32_t requested_size);

rust_heap_math_result_t rust_heap_realloc_copy_size(
	uint32_t block_size,
	uint32_t block_header_size,
	uint32_t new_size,
	uint32_t* out_copy_size,
	uint32_t* out_do_realloc);

#ifdef __cplusplus
}
#endif

#endif
