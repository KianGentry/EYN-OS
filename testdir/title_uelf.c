#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int read_line(char* out, int out_cap) {
    if (!out || out_cap <= 0) return 0;

    // Note: EYN-OS stdin read returns a whole line once Enter is pressed.
    // The kernel may include '\n' and/or a terminating NUL.
    int rc = (int)read(0, out, (size_t)(out_cap - 1));
    if (rc <= 0) {
        out[0] = '\0';
        return 0;
    }
    out[rc] = '\0';

    int n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[--n] = '\0';
    }
    return n;
}

int main(void) {
    puts("Type a new title + Enter. Type q + Enter to quit.");

    char line[96];
    for (;;) {
        int n = read_line(line, (int)sizeof(line));
        if (n == 1 && line[0] == 'q') {
            break;
        }
        int rc = gui_set_title(0, line);
        if (rc != 0) {
            printf("gui_set_title failed rc=%d title='%s'\n", rc, line);
        }
    }

    return 0;
}
