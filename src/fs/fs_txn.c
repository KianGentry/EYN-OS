#include <fs/fs_txn.h>
#include <string.h>

#define FS_TXN_DRIVE_MAX 4
#define FS_TXN_ENTRY_MAX 64

typedef struct fs_txn_state_t {
    uint32 depth;
    uint32 count;
    fs_txn_entry_t entries[FS_TXN_ENTRY_MAX];
} fs_txn_state_t;

static fs_txn_state_t g_fs_txn[FS_TXN_DRIVE_MAX];

void fs_txn_begin(uint8 drive) {
    if (drive >= FS_TXN_DRIVE_MAX) return;
    g_fs_txn[drive].depth++;
}

void fs_txn_commit(uint8 drive) {
    if (drive >= FS_TXN_DRIVE_MAX) return;
    if (g_fs_txn[drive].depth == 0) return;
    g_fs_txn[drive].depth--;
    if (g_fs_txn[drive].depth == 0) {
        g_fs_txn[drive].count = 0;
        memset(g_fs_txn[drive].entries, 0, sizeof(g_fs_txn[drive].entries));
    }
}

void fs_txn_abort(uint8 drive) {
    if (drive >= FS_TXN_DRIVE_MAX) return;
    g_fs_txn[drive].depth = 0;
    g_fs_txn[drive].count = 0;
    memset(g_fs_txn[drive].entries, 0, sizeof(g_fs_txn[drive].entries));
}

void fs_txn_touch(uint8 drive, uint32 tag, uint32 len) {
    if (drive >= FS_TXN_DRIVE_MAX) return;
    if (g_fs_txn[drive].depth == 0) return;
    if (g_fs_txn[drive].count >= FS_TXN_ENTRY_MAX) return;

    fs_txn_entry_t* e = &g_fs_txn[drive].entries[g_fs_txn[drive].count++];
    e->tag = tag;
    e->len = len;
}

int fs_txn_is_active(uint8 drive) {
    if (drive >= FS_TXN_DRIVE_MAX) return 0;
    return (g_fs_txn[drive].depth != 0) ? 1 : 0;
}

int fs_txn_get_count(uint8 drive) {
    if (drive >= FS_TXN_DRIVE_MAX) return 0;
    return (int)g_fs_txn[drive].count;
}
