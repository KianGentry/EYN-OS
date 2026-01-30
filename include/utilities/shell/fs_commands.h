#ifndef FS_COMMANDS_H
#define FS_COMMANDS_H
#include <misc/types.h>
#include <stdint.h>
#include <stddef.h>

void ls(string input);
void cat(string ch);
void read_cmd(string ch);
void del(string ch);
void write_cmd(string ch);
void writefat(string ch);
void catram(string ch);
void lsram(string input);
void writeram(string ch);
int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk);
int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk);
int check_filesystem_integrity(uint8_t disk);
void copy_cmd(string ch);
void move_cmd(string ch);
extern void* fat32_disk_img;
extern char shell_current_path[128];
void cd(string input);
void makedir(string ch);
void deldir(string ch);
void fscheck(string ch);
void resolve_path(const char* input, const char* cwd, char* out, size_t outsz);

#endif 