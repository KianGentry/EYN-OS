#ifndef EYN_INSTALL_PACKAGE_H
#define EYN_INSTALL_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#define MAX_NAME 64
#define MAX_URL 256
#define MAX_SHA 65
#define MAX_DEPS 16

typedef struct {
    char name[MAX_NAME];
    char version[MAX_NAME];
    char url[MAX_URL];
    char sha256[MAX_SHA];
    char deps[MAX_DEPS][MAX_NAME];
    int dep_count;
} Package;

struct PackageIndex;

int package_download_url_to_buffer(const char* url,
                                   uint8_t** out_data,
                                   size_t* out_len,
                                   size_t max_bytes);

int install_package(const struct PackageIndex* index, const Package* pkg);

#endif
