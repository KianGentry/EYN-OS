#pragma once

#include <stddef.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

void* malloc(size_t n);
void free(void* p);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* p, size_t n);

int atexit(void (*fn)(void));
char* getenv(const char* name);

void abort(void);
void exit(int code);

#ifndef RAND_MAX
#define RAND_MAX 32767
#endif

int rand(void);
void srand(unsigned int seed);

unsigned long strtoul(const char* nptr, char** endptr, int base);
long double strtold(const char* nptr, char** endptr);
long strtol(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);

int atoi(const char* s);
long atol(const char* s);

int    abs(int x);
long   labs(long x);
