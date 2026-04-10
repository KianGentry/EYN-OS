#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>

#include "../../install/index.h"
#include "../../install/package.h"
#include "../../install/resolve.h"

EYN_CMDMETA_V1("Install packages from the EYN-OS package index.", "install hello");

static void install_usage(void) {
    puts("Usage: install <package>\n"
         "Examples:\n"
         "  install hello");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        install_usage();
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        install_usage();
        return 0;
    }

    if (argc != 2) {
        install_usage();
        return 1;
    }

    const char* requested_name = argv[1];

    PackageIndex index;
    if (index_fetch_and_parse(&index) != 0) {
        return 1;
    }

    ResolvePlan plan;
    if (resolve_install_plan(&index, requested_name, &plan) != 0) {
        return 1;
    }

    if (plan.count <= 0) {
        printf("install: nothing to install for %s\n", requested_name);
        return 1;
    }

    printf("install: resolved %d package(s)\n", plan.count);

    for (int i = 0; i < plan.count; i++) {
        const Package* pkg = plan.ordered[i];
        printf("install: [%d/%d] %s@%s\n", i + 1, plan.count, pkg->name, pkg->version);

        if (install_package(&index, pkg) != 0) {
            printf("install: failed while installing %s\n", pkg->name);
            return 1;
        }
    }

    printf("install: complete (%d package(s))\n", plan.count);
    return 0;
}

/*
 * build_user_c.sh compiles one translation unit per command source.
 * Include install module implementations directly to preserve modular files
 * under /install without changing the userland build pipeline.
 */
#include "../../install/package.c"
#include "../../install/index.c"
#include "../../install/resolve.c"
