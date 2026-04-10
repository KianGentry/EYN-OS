#ifndef EYN_INSTALL_INDEX_H
#define EYN_INSTALL_INDEX_H

#include "package.h"

#define INSTALL_INDEX_URL "http://eynos.duckdns.org/index.json"
#define INSTALL_LOCAL_INDEX_PATH "/cache/index.json"
#define INDEX_MAX_PACKAGES 128
#define INDEX_MAX_JSON_BYTES (512u * 1024u)

typedef struct PackageIndex {
    Package packages[INDEX_MAX_PACKAGES];
    int count;
} PackageIndex;

int index_fetch_and_parse(PackageIndex* out_index);
const Package* index_find_package(const PackageIndex* index, const char* name);
int index_has_package(const PackageIndex* index, const char* name);

#endif
