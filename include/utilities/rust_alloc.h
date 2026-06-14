#ifndef RUST_ALLOC_H
#define RUST_ALLOC_H

#include <misc/types.h>
#include <utilities/zero_copy.h>

#ifdef __cplusplus
extern "C" {
#endif

zero_copy_buffer_t* rust_zero_copy_buffer_create(uint32_t size);
void rust_zero_copy_buffer_destroy(zero_copy_buffer_t* buffer);
int rust_zero_copy_buffer_read(const zero_copy_buffer_t* buffer, void* data, uint32_t offset, uint32_t size);
int rust_zero_copy_buffer_write(zero_copy_buffer_t* buffer, const void* data, uint32_t offset, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
