#ifndef RUST_EYNFS_H
#define RUST_EYNFS_H

#include <misc/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t block_num;
	uint32_t last_use;
	uint8_t* data;
	uint8_t dirty;
	uint8_t valid;
} rust_eynfs_cache_entry_t;

typedef enum {
	RUST_EYNFS_CACHE_VALID = 0,
	RUST_EYNFS_CACHE_BAD_TABLE_PTR = 1,
	RUST_EYNFS_CACHE_BAD_DATA_PTR = 2,
	RUST_EYNFS_CACHE_BAD_CAPACITY = 3,
	RUST_EYNFS_CACHE_BAD_ENTRY_PTR = 4,
	RUST_EYNFS_CACHE_BAD_PHYSMAP = 5
} rust_eynfs_cache_validation_result_t;

typedef enum {
	RUST_EYNFS_VICTIM_NONE = 0,
	RUST_EYNFS_VICTIM_FOUND_CLEAN = 1,
	RUST_EYNFS_VICTIM_FOUND_DIRTY = 2,
	RUST_EYNFS_VICTIM_FOUND_INVALID = 3
} rust_eynfs_victim_result_t;

rust_eynfs_cache_validation_result_t rust_eynfs_validate_block_cache(
	const rust_eynfs_cache_entry_t* entries,
	const uint8_t* data_base,
	uint32_t capacity,
	uintptr kernel_base,
	uintptr physmap_end,
	uint32_t block_size);

rust_eynfs_victim_result_t rust_eynfs_choose_victim_index(
	const rust_eynfs_cache_entry_t* entries,
	uint32_t capacity,
	uint32_t* out_index);

#ifdef __cplusplus
}
#endif

#endif