#ifndef STRING_H
#define STRING_H

#include <misc/types.h>
#include <stddef.h>

// Standard C library string functions
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
char *strpbrk(const char *s, const char *accept);
char *strtok_r(char *str, const char *delim, char **saveptr);

// Standard C library memory functions
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

// EYN-OS specific string utilities
char* safe_strcpy(char* dest, const char* src, int dest_size);
char* int_to_string(int n);
int str_to_int(const char* str);
uint16 strlength(string ch);
uint8 cmdEql(const char* ch1, const char* ch2);
uint8 strEql(const char* ch1, const char* ch2);
int parse_redirection(const char* input, char* cmd, char* filename);
uint32 str_to_uint(const char* s);
int atoi(const char* s);
int validate_file_path(const char* path);
char* sanitize_input(char* input, int max_length);
int get_input_validation_errors(void);

// Safe string functions with bounds checking
char *strcpy_safe(char *dest, const char *src, size_t dest_size);
char *strncpy_safe(char *dest, const char *src, size_t n, size_t dest_size);
char *strcat_safe(char *dest, const char *src, size_t dest_size);
char *strncat_safe(char *dest, const char *src, size_t n, size_t dest_size);
int strcasecmp(const char* a, const char* b);
#endif
