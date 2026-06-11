#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include <misc/types.h>

#define SYS_CFG_LABEL_MAX 15

void system_config_init_defaults(void);
int system_config_load(uint8 drive);
int system_config_save(void);
void system_config_set_save_drive(uint8 drive);

uint8 system_config_get_install_drive_logical(void);
uint8 system_config_get_install_drive_physical(void);
int system_config_set_install_drive_logical(uint8 logical_drive);

const char* system_config_get_install_bin_path(void);
int system_config_set_install_bin_path(const char* path);

int system_config_validate_label(const char* label);
const char* system_config_get_drive_label_ptr(uint8 logical_drive);
int system_config_get_drive_label(uint8 logical_drive, char* out, int out_cap);
int system_config_set_drive_label(uint8 logical_drive, const char* label);

#endif