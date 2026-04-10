#ifndef EYN_INSTALL_RESOLVE_H
#define EYN_INSTALL_RESOLVE_H

#include "index.h"

typedef struct {
    const Package* ordered[INDEX_MAX_PACKAGES];
    int count;
} ResolvePlan;

int resolve_install_plan(const PackageIndex* index,
                         const char* root_name,
                         ResolvePlan* out_plan);

#endif
