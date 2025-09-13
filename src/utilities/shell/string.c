#include <string.h>
#include <types.h>
#include <vga.h>

// Standard C library string functions
size_t strlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
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
int validate_string(const char* str, int max_length) {
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

// Copies the argument after 'echo' to outbuf
void echo_to_buf(string ch, char* outbuf, int outbufsize) {
    int i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] == ' ') i++;
    int j = 0;
    while (ch[i] && j < outbufsize - 1) outbuf[j++] = ch[i++];
    outbuf[j] = '\0';
}

// Evaluates a simple integer expression after 'calc' and writes result to outbuf
void calc_to_buf(string str, char* outbuf, int outbufsize) {
    int i = 0;
    while (str[i] && str[i] != ' ') i++;
    while (str[i] == ' ') i++;
    // Only support: calc <int> <op> <int>
    int a = 0, b = 0;
    char op = 0;
    int sign = 1;
    if (str[i] == '-') { sign = -1; i++; }
    while (str[i] >= '0' && str[i] <= '9') { a = a * 10 + (str[i++] - '0'); }
    a *= sign;
    while (str[i] == ' ') i++;
    op = str[i++];
    while (str[i] == ' ') i++;
    sign = 1;
    if (str[i] == '-') { sign = -1; i++; }
    while (str[i] >= '0' && str[i] <= '9') { b = b * 10 + (str[i++] - '0'); }
    b *= sign;
    int res = 0;
    switch (op) {
        case '+': res = a + b; break;
        case '-': res = a - b; break;
        case '*': res = a * b; break;
        case '/': res = (b != 0) ? a / b : 0; break;
        default: {
            const char* msg = "Invalid op";
            int j = 0;
            while (msg[j] && j < outbufsize - 1) { outbuf[j] = msg[j]; j++; }
            outbuf[j] = '\0';
            return;
        }
    }
    string tmp = int_to_string(res);
    int j = 0;
    while (tmp[j] && j < outbufsize - 1) { outbuf[j] = tmp[j]; j++; }
    outbuf[j] = '\0';
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

uint8 cmdLength(string ch) {
        if (!ch) return 0;
        
        uint8 i = 0;
        uint8 size = 0;
        uint8 realSize = strlen(ch);
        uint8 hasSpace = 0;

        // If the string is empty, return 0
        if (realSize == 0) return 0;

        // If there's no space, return the full length
        for (i = 0; i < realSize; i++) {
            if (ch[i] == ' ') {
                hasSpace = 1;
                break;
            }
            size++;
        }
        
        // Use hasSpace to determine return value
        return hasSpace ? size : realSize;
}

uint8 isStringEmpty(string ch1) {
        if (ch1[0] == '1') {return 0;}
        else if (ch1[0] == '2') {return 0;}
        else if (ch1[0] == '3') {return 0;}
        else if (ch1[0] == '4') {return 0;}
        else if (ch1[0] == '5') {return 0;}
        else if (ch1[0] == '6') {return 0;}
        else if (ch1[0] == '7') {return 0;}
        else if (ch1[0] == '8') {return 0;}
        else if (ch1[0] == '9') {return 0;}
        else if (ch1[0] == '0') {return 0;}
        else if (ch1[0] == '-') {return 0;}
        else if (ch1[0] == '=') {return 0;}
        else if (ch1[0] == 'q') {return 0;}
        else if (ch1[0] == 'w') {return 0;}
        else if (ch1[0] == 'e') {return 0;}
        else if (ch1[0] == 'r') {return 0;}
        else if (ch1[0] == 't') {return 0;}
        else if (ch1[0] == 'y') {return 0;}
        else if (ch1[0] == 'u') {return 0;}
        else if (ch1[0] == 'i') {return 0;}
        else if (ch1[0] == 'o') {return 0;}
        else if (ch1[0] == 'p') {return 0;}
        else if (ch1[0] == '[') {return 0;}
        else if (ch1[0] == ']') {return 0;}
        else if (ch1[0] == '\\') {return 0;}
        else if (ch1[0] == 'a') {return 0;}
        else if (ch1[0] == 's') {return 0;}
        else if (ch1[0] == 'd') {return 0;}
        else if (ch1[0] == 'f') {return 0;}
        else if (ch1[0] == 'g') {return 0;}
        else if (ch1[0] == 'h') {return 0;}
        else if (ch1[0] == 'j') {return 0;}
        else if (ch1[0] == 'k') {return 0;}
        else if (ch1[0] == 'l') {return 0;}
        else if (ch1[0] == ';') {return 0;}
        else if (ch1[0] == '\'') {return 0;}
        else if (ch1[0] == 'z') {return 0;}
        else if (ch1[0] == 'x') {return 0;}
        else if (ch1[0] == 'c') {return 0;}
        else if (ch1[0] == 'v') {return 0;}
        else if (ch1[0] == 'b') {return 0;}
        else if (ch1[0] == 'n') {return 0;}
        else if (ch1[0] == 'm') {return 0;}
        else if (ch1[0] == ',') {return 0;}
        else if (ch1[0] == '.') {return 0;}
        else if (ch1[0] == '/') {return 0;}
        else {
                return 1;
        }
}

// Additional utility functions for EYN-OS
uint8 strEql(string ch1, string ch2) {
    if (!ch1 || !ch2) return 0;
    return strcmp(ch1, ch2) == 0;
}

uint8 cmdEql(string ch1, string ch2) {
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