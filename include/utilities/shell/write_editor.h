#ifndef WRITE_EDITOR_H
#define WRITE_EDITOR_H
#include <misc/types.h>

void write_editor(const char* filename, uint8 disk);
int load_file_to_write_editor(const char* filename, uint8 disk);
int save_write_editor_buffer(const char* filename, uint8 disk);

#endif 