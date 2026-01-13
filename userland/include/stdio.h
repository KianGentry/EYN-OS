#pragma once

#include <stddef.h>
#include <stdarg.h>

#ifndef EOF
#define EOF (-1)
#endif

typedef struct FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);

int fputc(int c, FILE* f);
int fputs(const char* s, FILE* f);
int fflush(FILE* f);

int vfprintf(FILE* f, const char* fmt, va_list ap);
int fprintf(FILE* f, const char* fmt, ...);
int printf(const char* fmt, ...);

// GNU-style; used by chibicc for buffering.
FILE* open_memstream(char** bufp, size_t* sizep);

int putchar(int ch);
int puts(const char* s);
