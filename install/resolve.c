#include "resolve.h"

#include <stdio.h>
#include <string.h>

#define RES_STATE_UNVISITED 0
#define RES_STATE_VISITING 1
#define RES_STATE_DONE 2

static int res_find_package_index(const PackageIndex* index, const char* name) {
    if (!index || !name || !name[0]) return -1;

    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->packages[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static int res_visit(const PackageIndex* index,
                     int package_idx,
                     uint8_t state[INDEX_MAX_PACKAGES],
                     ResolvePlan* out_plan) {
    if (state[package_idx] == RES_STATE_DONE) {
        return 0;
    }

    if (state[package_idx] == RES_STATE_VISITING) {
        printf("install: dependency cycle at %s\n", index->packages[package_idx].name);
        return -1;
    }

    state[package_idx] = RES_STATE_VISITING;

    const Package* pkg = &index->packages[package_idx];
    for (int i = 0; i < pkg->dep_count; i++) {
        const char* dep_name = pkg->deps[i];
        int dep_idx = res_find_package_index(index, dep_name);
        if (dep_idx < 0) {
            printf("install: missing dependency '%s' required by '%s'\n", dep_name, pkg->name);
            return -1;
        }

        if (res_visit(index, dep_idx, state, out_plan) != 0) {
            return -1;
        }
    }

    state[package_idx] = RES_STATE_DONE;

    if (out_plan->count >= INDEX_MAX_PACKAGES) {
        puts("install: dependency plan exceeded maximum size");
        return -1;
    }

    out_plan->ordered[out_plan->count++] = pkg;
    return 0;
}

int resolve_install_plan(const PackageIndex* index,
                         const char* root_name,
                         ResolvePlan* out_plan) {
    if (!index || !root_name || !root_name[0] || !out_plan) return -1;

    memset(out_plan, 0, sizeof(*out_plan));

    int root_idx = res_find_package_index(index, root_name);
    if (root_idx < 0) {
        printf("install: package not found in index: %s\n", root_name);
        return -1;
    }

    uint8_t state[INDEX_MAX_PACKAGES];
    memset(state, 0, sizeof(state));

    if (res_visit(index, root_idx, state, out_plan) != 0) {
        return -1;
    }

    return 0;
}
