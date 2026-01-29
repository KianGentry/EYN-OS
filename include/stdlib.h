#ifndef EYNOS_STDLIB_SHIM_H
#define EYNOS_STDLIB_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t nmemb, size_t size);

int atoi(const char* s);

#ifdef __cplusplus
}
#endif

#endif
