#ifndef EYN_INSTALL_PACKAGE_H
#define EYN_INSTALL_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#define MAX_NAME 64
#define MAX_URL 256
#define MAX_SHA 65
#define MAX_DEPS 16
#define MAX_INSTALL_DIR 96

typedef struct {
    char name[MAX_NAME];
    char version[MAX_NAME];
    char url[MAX_URL];
    char sha256[MAX_SHA];
    char install_dir[MAX_INSTALL_DIR];
    char install_name[MAX_NAME];
    char deps[MAX_DEPS][MAX_NAME];
    int dep_count;
} Package;

#define PACKAGE_CONFIRM_PROMPT 0
#define PACKAGE_CONFIRM_AUTO_ACCEPT 1

struct PackageIndex;

int package_download_url_to_buffer(const char* url,
                                   uint8_t** out_data,
                                   size_t* out_len,
                                   size_t max_bytes);

int package_set_confirm_mode(int mode);

int install_package(const struct PackageIndex* index, const Package* pkg);

#endif
