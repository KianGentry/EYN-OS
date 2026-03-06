#include <stddef.h>

size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);

char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strncat(char* dst, const char* src, size_t n);

char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);

char* strtok(char* str, const char* delim);

char* strdup(const char* s);
char* strndup(const char* s, size_t n);

void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int v, size_t n);
int memcmp(const void* a, const void* b, size_t n);

