#ifndef PANIC_H
#define PANIC_H

#include <types.h>

void panic(const char* msg, const char* file, int line);
void panicf(const char* file, int line, const char* fmt, ...);
void assert_fail(const char* expr, const char* file, int line);
void backtrace(void);
int panic_is_in_progress(void);

#define PANIC(msg) panic((msg), __FILE__, __LINE__)
#define PANICF(fmt, ...) panicf(__FILE__, __LINE__, (fmt), __VA_ARGS__)
#define ASSERT(x) do { if (!(x)) assert_fail(#x, __FILE__, __LINE__); } while (0)

#endif
