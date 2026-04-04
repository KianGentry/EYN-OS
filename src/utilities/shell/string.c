#include <string.h>
#include <misc/types.h>
#include <serial.h>
#include <vga.h>

static void serial_write_hex_uintptr(uintptr value) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = (EYNOS_POINTER_BITS / 4) - 1; i >= 0; --i) {
        uint8 nib = (uint8)((value >> (i * 4)) & 0xFu);
        (void)serial_write_char(SERIAL_COM1, hex[nib]);
    }
}

// Standard C library string functions
size_t strlen(const char *s) {
    if (!s) return 0;

    // Guard against obvious bad pointers (e.g. COM1 port 0x3F8 mistakenly passed
    // as a string pointer via varargs or memory corruption). This avoids taking
    // a page fault inside strlen and helps identify the caller.
    if ((uintptr)s < 0x1000u) {
        static int g_strlen_reporting = 0;
        if (!g_strlen_reporting) {
            g_strlen_reporting = 1;

            const char *prefix = "[strlen] bad ptr=0x";
            for (const char *p = prefix; *p; ++p) (void)serial_write_char(SERIAL_COM1, *p);

            serial_write_hex_uintptr((uintptr)s);

            const char *mid = " ra=0x";
            for (const char *p = mid; *p; ++p) (void)serial_write_char(SERIAL_COM1, *p);

            serial_write_hex_uintptr((uintptr)__builtin_return_address(0));

            (void)serial_write_char(SERIAL_COM1, '\n');

            g_strlen_reporting = 0;
        }
        return 0;
    }

        // Bound scanning to avoid hard hangs if a corrupted pointer references memory
        // without a terminating NUL in reasonable range.
        const uint32 kMaxScan = 16384;
        const char *p = s;
        for (uint32 i = 0; i < kMaxScan; ++i) {
            if (*p == '\0') return (size_t)(p - s);
            ++p;
        }

        // If we hit the limit, emit a breadcrumb and return the capped length.
        {
            const char *prefix = "[strlen] no-nul ptr=0x";
            for (const char *q = prefix; *q; ++q) (void)serial_write_char(SERIAL_COM1, *q);
            serial_write_hex_uintptr((uintptr)s);

            const char *mid = " ra=0x";
            for (const char *q = mid; *q; ++q) (void)serial_write_char(SERIAL_COM1, *q);
            serial_write_hex_uintptr((uintptr)__builtin_return_address(0));
            (void)serial_write_char(SERIAL_COM1, '\n');
        }
        return (size_t)kMaxScan;
}

char *strcpy(char *dest, const char *src) {
    if (!dest || !src) return dest;
    
    char *d = dest;
    while (*src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    if (!dest || !src) return dest;
    
    char *d = dest;
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        d[i] = src[i];
    }
    for (; i < n; i++) {
        d[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    if (!dest || !src) return dest;
    
    char *d = dest;
    while (*d) d++;
    while (*src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    if (!dest || !src) return dest;
    
    char *d = dest;
    while (*d) d++;
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        d[i] = src[i];
    }
    d[i] = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(*s1 - *s2);
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;
    
    size_t i;
    for (i = 0; i < n && s1[i] && s2[i] && s1[i] == s2[i]; i++);
    
    if (i == n) return 0;
    return (int)(s1[i] - s2[i]);
}

char *strchr(const char *s, int c) {
    if (!s) return NULL;
    
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    if ((char)c == '\0') return (char*)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    if (!s) return NULL;
    
    char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = (char*)s;
        s++;
    }
    if ((char)c == '\0') last = (char*)s;
    return last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return (char*)haystack;
    
    size_t needle_len = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;
        }
        haystack++;
    }
    return NULL;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!delim || !saveptr) return NULL;
    
    char *token_start;
    if (str) {
        *saveptr = str;
    } else if (!*saveptr || **saveptr == '\0') {
        return NULL;
    }
    
    // Skip leading delimiters
    while (**saveptr && strchr(delim, **saveptr)) {
        (*saveptr)++;
    }
    
    if (**saveptr == '\0') {
        return NULL;
    }
    
    token_start = *saveptr;
    
    // Find end of token
    while (**saveptr && !strchr(delim, **saveptr)) {
        (*saveptr)++;
    }
    
    if (**saveptr != '\0') {
        **saveptr = '\0';
        (*saveptr)++;
    }
    
    return token_start;
}

// EYN-OS specific string utilities
char* safe_strcpy(char* dest, const char* src, int dest_size) {
    if (!dest || !src || dest_size <= 0) return dest;
    
    int i = 0;
    while (i < dest_size - 1 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
    return dest;
}

char* int_to_string(int n) {
    static char buffer[32];
    int i = 0;
    int is_negative = 0;
    
    if (n == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return buffer;
    }
    
    if (n < 0) {
        is_negative = 1;
        n = -n;
    }
    
    while (n > 0) {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }
    
    if (is_negative) {
        buffer[i++] = '-';
    }
    
    // Reverse the string
    for (int j = 0; j < i / 2; j++) {
        char temp = buffer[j];
        buffer[j] = buffer[i - 1 - j];
        buffer[i - 1 - j] = temp;
    }
    
    buffer[i] = '\0';
    return buffer;
}

int str_to_int(const char* str) {
    if (!str) return 0;
    
    int result = 0;
    int sign = 1;
    int i = 0;
    
    // Handle sign
    if (str[i] == '-') {
        sign = -1;
        i++;
    } else if (str[i] == '+') {
        i++;
    }
    
    // Convert digits
    while (str[i] >= '0' && str[i] <= '9') {
        result = result * 10 + (str[i] - '0');
        i++;
    }
    
    return result * sign;
}

// Memory functions are implemented in util.c

// EYN-OS specific string functions
uint16 strlength(string ch) {
    if (!ch) return 0;
    uint16 i = 0;
    while (ch[i]) i++;
    return i;
}

// Input validation and buffer overflow protection
static int input_validation_errors = 0;

// Validate string bounds and content
static int validate_string(const char* str, int max_length) {
    if (!str) return 0;
    
    // Check for null bytes and excessive length
    for (int i = 0; i < max_length; i++) {
        if (str[i] == '\0') break;
        if (str[i] < 32 || str[i] > 126) {
            // Invalid character (outside printable ASCII)
            return 0;
        }
    }
    
    return 1;
}

// Validate file path for traversal attacks
int validate_file_path(const char* path) {
    if (!path) return 0;
    
    // Check for path traversal attempts
    if (strstr(path, "..") || strstr(path, "//") || strstr(path, "\\")) {
        return 0; // Potential path traversal
    }
    
    // Check for absolute paths (should be relative)
    if (path[0] == '/') {
        return 0; // Absolute path not allowed
    }
    
    // Validate string content
    return validate_string(path, 256);
}

// Get input validation error count
int get_input_validation_errors() {
    return input_validation_errors;
}

// Parses redirection: e.g. "echo hi > file.txt" -> cmd="echo hi", filename="file.txt"
int parse_redirection(const char* input, char* cmd, char* filename) {
    int i = 0, j = 0, k = 0;
    int found = 0;
    
    // Add bounds checking for cmd buffer (assuming 256 bytes like other buffers)
    while (input[i] && j < 255) {
        if (input[i] == '>') {
            found = 1;
            break;
        }
        cmd[j++] = input[i++];
    }
    cmd[j] = '\0';
    if (!found) return 0;
    i++; // skip '>'
    while (input[i] == ' ') i++;
    while (input[i] && input[i] != ' ' && k < 63) filename[k++] = input[i++];
    filename[k] = '\0';
    return 1;
}

// Helper functions for strtok_r
size_t strspn(const char *s, const char *accept) {
    const char *p;
    size_t count = 0;
    while (*s) {
        for (p = accept; *p; p++) {
            if (*s == *p) break;
        }
        if (!*p) break;
        s++; count++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
        s++;
    }
    return NULL;
}

// Additional utility functions for EYN-OS
uint8 strEql(const char* ch1, const char* ch2) {
    if (!ch1 || !ch2) return 0;
    return strcmp(ch1, ch2) == 0;
}

uint8 cmdEql(const char* ch1, const char* ch2) {
    if (!ch1 || !ch2) return 0;
    return strcmp(ch1, ch2) == 0;
}

// Converts a string to uint32 (decimal only)
uint32 str_to_uint(const char* s) {
    if (!s) return 0;
    uint32 n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

// Converts a string to integer (decimal only)
int atoi(const char* s) {
    if (!s) return 0;
    return str_to_int(s);
}

// Simple freestanding implementation of case-insensitive strcmp for small paths
int strcasecmp(const char* astr, const char* bstr) {
    while (*astr && *bstr) {
        char ca = *astr;
        char cb = *bstr;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return (int)((unsigned char)ca) - (int)((unsigned char)cb);
        astr++; bstr++;
    }
    return (int)((unsigned char)*astr) - (int)((unsigned char)*bstr);
}