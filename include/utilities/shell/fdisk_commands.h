#ifndef FDISK_COMMANDS_H
#define FDISK_COMMANDS_H
#include <misc/types.h>
#include <utilities/shell/shell_args.h>

void fdisk_list();
void fdisk_create_partition(uint32 start_lba, uint32 size, uint8 type);
void fdisk_cmd_handler(const shell_args_t* args);

#endif 