#ifndef FS_TXN_H
#define FS_TXN_H

#include <misc/types.h>

#define FS_TXN_TAG_SUPERBLOCK 1u
#define FS_TXN_TAG_DIR        2u
#define FS_TXN_TAG_FILE       3u

typedef struct fs_txn_entry_t {
    uint32 tag;
    uint32 len;
} fs_txn_entry_t;

void fs_txn_begin(uint8 drive);
void fs_txn_commit(uint8 drive);
void fs_txn_abort(uint8 drive);
void fs_txn_touch(uint8 drive, uint32 tag, uint32 len);
int fs_txn_is_active(uint8 drive);
int fs_txn_get_count(uint8 drive);

#endif // FS_TXN_H
