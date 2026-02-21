#include <string.h>
#include <vga.h>
#include <util.h>
#include <multiboot.h>
#include <math.h>
#include <misc/types.h>

extern multiboot_info_t *g_mbi;

int32_t math_add(int32_t num1, int32_t num2) {
	return (num1 + num2);
}

int32_t math_remove(int32_t num1, int32_t num2) {
	return (num1 - num2);
}

int32_t math_epi(int32_t num1, int32_t num2) {
	// Prevent overflow by dividing first
	int32_t temp = num1 / FIXED_POINT_FACTOR;
	return temp * num2;
}

int32_t math_dia(int32_t num1, int32_t num2) {
	if (num2 == 0) return 0; // Prevent division by zero
	// Prevent overflow by dividing first
	return (num1 / num2) * FIXED_POINT_FACTOR;
}

int32_t math_get_current_equation(string str) {
	if (!str) return 0;
	
	uint8 size = strlength(str);
	if (size == 0) return 0;
	
	int32_t num1 = 0;
	int32_t num2 = 0;
	uint8 operation = 0;
	uint8 i = 0;
	uint8 found_operation = 0;
	uint8 decimal_digits = 0;
	
	// Skip leading spaces
	while (i < size && str[i] == ' ') i++;
	
	// Parse first number
	while (i < size) {
		if (str[i] == '.') {
			if (decimal_digits > 0) return 0; // Multiple decimal points
			decimal_digits = 1;
		}
		else if (str[i] >= '0' && str[i] <= '9') {
			// Check for overflow before multiplying
			if (num1 > INT32_MAX / 10) return 0;
			num1 = num1 * 10 + (str[i] - '0');
			if (decimal_digits > 0) {
				if (decimal_digits > 3) break; // Limit to 3 decimal places
				decimal_digits++;
			}
		}
		else if (str[i] == ' ') {
			break;
		}
		else {
			break;
		}
		i++;
	}
	
	// Apply decimal places to first number
	if (decimal_digits > 0) {
		// Convert to fixed-point by multiplying by appropriate power of 10
		while (decimal_digits < 4) {
			if (num1 > INT32_MAX / 10) return 0;
			num1 *= 10;
			decimal_digits++;
		}
	} else {
		if (num1 > INT32_MAX / FIXED_POINT_FACTOR) return 0;
		num1 *= FIXED_POINT_FACTOR;
	}
	
	// Skip spaces
	while (i < size && str[i] == ' ') i++;
	
	// Get operation
	if (i >= size) return 0;
	
	switch (str[i]) {
		case '+': operation = 0; found_operation = 1; break;
		case '-': operation = 1; found_operation = 1; break;
		case '*': operation = 2; found_operation = 1; break;
		case '/': operation = 3; found_operation = 1; break;
		default: return 0;
	}
	i++;
	
	// Skip spaces
	while (i < size && str[i] == ' ') i++;
	
	// Reset decimal parsing for second number
	decimal_digits = 0;
	
	// Parse second number
	while (i < size) {
		if (str[i] == '.') {
			if (decimal_digits > 0) return 0; // Multiple decimal points
			decimal_digits = 1;
		}
		else if (str[i] >= '0' && str[i] <= '9') {
			// Check for overflow before multiplying
			if (num2 > INT32_MAX / 10) return 0;
			num2 = num2 * 10 + (str[i] - '0');
			if (decimal_digits > 0) {
				if (decimal_digits > 3) break; // Limit to 3 decimal places
				decimal_digits++;
			}
		}
		else if (str[i] == ' ') {
			break;
		}
		else {
			break;
		}
		i++;
	}
	
	// Apply decimal places to second number
	if (decimal_digits > 0) {
		// Convert to fixed-point by multiplying by appropriate power of 10
		while (decimal_digits < 4) {
			if (num2 > INT32_MAX / 10) return 0;
			num2 *= 10;
			decimal_digits++;
		}
	} else {
		if (num2 > INT32_MAX / FIXED_POINT_FACTOR) return 0;
		num2 *= FIXED_POINT_FACTOR;
	}
	
	// Skip trailing spaces
	while (i < size && str[i] == ' ') i++;
	
	// Check if we've processed the entire string
	if (i < size || !found_operation) return 0;
	
	// Perform calculation
	switch (operation) {
		case 0: return math_add(num1, num2);
		case 1: return math_remove(num1, num2);
		case 2: return math_epi(num1, num2);
		case 3: return math_dia(num1, num2);
		default: return 0;
	}
}

// Random number generator state
static uint32_t rand_state = 1;

// Seed the random number generator
void rand_seed(uint32_t seed) {
	rand_state = seed;
}

// Generate next random number using Linear Congruential Generator
// Using constants from glibc's rand() implementation
uint32_t rand_next(void) {
	rand_state = rand_state * 1103515245 + 12345;
	return (rand_state >> 16) & 0x7FFF;
}

// Generate random number in range [min, max]
uint32_t rand_range(uint32_t min, uint32_t max) {
	if (min >= max) return min;
	
	uint32_t range = max - min + 1;
	uint32_t random = rand_next();
	
	// Scale the random number to the desired range
	return min + (random % range);
}

// Swap two string pointers
void swap_strings(char** a, char** b) {
    char* temp = *a;
    *a = *b;
    *b = temp;
}

// Partition function for quicksort
int partition_strings(char** arr, int low, int high) {
    char* pivot = arr[high];
    int i = (low - 1);
    
    for (int j = low; j <= high - 1; j++) {
        if (strcmp(arr[j], pivot) <= 0) {
            i++;
            swap_strings(&arr[i], &arr[j]);
        }
    }
    swap_strings(&arr[i + 1], &arr[high]);
    return (i + 1);
}

// Quicksort implementation for strings
void quicksort_strings(char** arr, int low, int high) {
    if (low < high) {
        int pi = partition_strings(arr, low, high);
        quicksort_strings(arr, low, pi - 1);
        quicksort_strings(arr, pi + 1, high);
    }
}

// Boyer-Moore search algorithm implementation

// Build bad character table for Boyer-Moore
void build_bad_char_table(const char* pattern, int pattern_len, int* bad_char_table) {
    // Initialize all entries to -1
    for (int i = 0; i < 256; i++) {
        bad_char_table[i] = -1;
    }
    
    // Fill the actual values
    for (int i = 0; i < pattern_len - 1; i++) {
        bad_char_table[(unsigned char)pattern[i]] = i;
    }
}

// Build good suffix table for Boyer-Moore
void build_good_suffix_table(const char* pattern, int pattern_len, int* good_suffix_table) {
    // Initialize all entries to pattern_len
    for (int i = 0; i < pattern_len; i++) {
        good_suffix_table[i] = pattern_len;
    }
    
    // Case 1: Exact match
    good_suffix_table[pattern_len - 1] = 1;
    
    // Case 2: Good suffix exists
    for (int i = pattern_len - 2; i >= 0; i--) {
        int j = 0;
        while (j < pattern_len - 1 - i && 
               pattern[pattern_len - 1 - j] == pattern[pattern_len - 1 - i - j]) {
            j++;
        }
        if (j == pattern_len - 1 - i) {
            good_suffix_table[i] = pattern_len - 1 - i;
        }
    }
    
    // Case 3: Prefix of pattern matches suffix of good suffix
    for (int i = 0; i < pattern_len - 1; i++) {
        int j = 0;
        while (j <= i && pattern[j] == pattern[pattern_len - 1 - i + j]) {
            j++;
        }
        if (j > i) {
            good_suffix_table[pattern_len - 1 - i] = pattern_len - 1 - i;
        }
    }
}

// Boyer-Moore search function
int boyer_moore_search(const char* text, const char* pattern) {
    int text_len = strlen(text);
    int pattern_len = strlen(pattern);
    
    if (pattern_len == 0) return 0;
    if (text_len == 0) return -1;
    if (pattern_len > text_len) return -1;
    
    // Allocate tables
    int bad_char_table[256];
    int* good_suffix_table = (int*)malloc(pattern_len * sizeof(int));
    
    if (!good_suffix_table) {
        return -1; // Memory allocation failed
    }
    
    // Build tables
    build_bad_char_table(pattern, pattern_len, bad_char_table);
    build_good_suffix_table(pattern, pattern_len, good_suffix_table);
    
    // Search
    int i = pattern_len - 1;
    while (i < text_len) {
        int j = pattern_len - 1;
        int k = i;
        
        // Compare pattern from right to left
        while (j >= 0 && text[k] == pattern[j]) {
            k--;
            j--;
        }
        
        if (j < 0) {
            // Pattern found
            free(good_suffix_table);
            return k + 1;
        } else {
            // Calculate shift
            int bad_char_shift = j - bad_char_table[(unsigned char)text[k]];
            int good_suffix_shift = good_suffix_table[j];
            
            // Use the larger shift
            int shift = (bad_char_shift > good_suffix_shift) ? bad_char_shift : good_suffix_shift;
            if (shift <= 0) shift = 1; // Ensure we make progress
            
            i += shift;
        }
    }
    
    free(good_suffix_table);
    return -1; // Pattern not found
}