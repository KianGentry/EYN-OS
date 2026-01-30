#ifndef MATH_H
#define MATH_H

#include <misc/types.h>
#include <stdint.h>

// Fixed-point arithmetic with 3 decimal places
#define FIXED_POINT_FACTOR 1000

// Random number generator constants
#define RAND_MAX 0x7FFFFFFF

int32_t math_add(int32_t num1, int32_t num2);
int32_t math_remove(int32_t num1, int32_t num2);
int32_t math_epi(int32_t num1, int32_t num2);
int32_t math_dia(int32_t num1, int32_t num2);
int32_t math_get_current_equation(string str);

// Random number generator functions
void rand_seed(uint32_t seed);
uint32_t rand_next(void);
uint32_t rand_range(uint32_t min, uint32_t max);

// Sorting functions
void quicksort_strings(char** arr, int low, int high);
int partition_strings(char** arr, int low, int high);
void swap_strings(char** a, char** b);

// Boyer-Moore search algorithm functions
int boyer_moore_search(const char* text, const char* pattern);
void build_bad_char_table(const char* pattern, int pattern_len, int* bad_char_table);
void build_good_suffix_table(const char* pattern, int pattern_len, int* good_suffix_table);

#endif