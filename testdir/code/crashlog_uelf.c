#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Inspect and clear crashlog records.", "crashlog dump");

static const char* obj_name(uint32_t obj_type) {
    if (obj_type == 1u) return "ui_prefs";
    return "unknown";
}

static void print_hex(const uint8_t* data, int len, int max_len) {
    int limit = (len < max_len) ? len : max_len;
    for (int i = 0; i < limit; ++i) {
        printf("%02x", (unsigned)data[i]);
        if (i + 1 < limit) putchar(' ');
    }
    if (len > limit) printf(" ...");
    putchar('\n');
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1]) {
        puts("Usage: crashlog dump | crashlog clear yes");
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        puts("Usage: crashlog dump | crashlog clear yes");
        return 0;
    }
    if (strcmp(argv[1], "dump") == 0) {
        int count = eyn_sys_crashlog_count();
        if (count < 0) {
            puts("crashlog: failed to query records");
            return 1;
        }
        if (count == 0) {
            puts("Crashlog: empty");
            return 0;
        }

        printf("Crashlog records: %d\n", count);
        for (int i = 0; i < count; ++i) {
            eyn_crashlog_record_info_t info;
            if (eyn_sys_crashlog_info((uint32_t)i, &info) != 0) {
                printf("  [%d] invalid\n", i);
                continue;
            }

            printf("  [%d] type=%u (%s) id=%u epoch=%u len=%u checksum=%08x\n",
                   i,
                   (unsigned)info.obj_type,
                   obj_name(info.obj_type),
                   (unsigned)info.obj_id,
                   (unsigned)info.epoch,
                   (unsigned)info.data_len,
                   (unsigned)info.checksum);

            uint8_t data[128];
            int n = eyn_sys_crashlog_data((uint32_t)i, data, (int)sizeof(data));
            if (n > 0) {
                printf("       data: ");
                print_hex(data, n, 16);
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        if (argc >= 3 && argv[2] && strcmp(argv[2], "yes") == 0) {
            if (eyn_sys_crashlog_clear() != 0) {
                puts("crashlog: failed to clear records");
                return 1;
            }
            puts("Crashlog cleared.");
            return 0;
        }
        puts("Usage: crashlog clear yes");
        return 1;
    }
    puts("Usage: crashlog dump | crashlog clear yes");
    return 1;
}
