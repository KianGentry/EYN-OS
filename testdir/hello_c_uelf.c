#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("Hello from C (.uelf)!\n");
    printf("Press q to quit.\n");

    for (;;) {
        int ch = getkey();
        if (ch == 'q') {
            break;
        }
    }

    _exit(0);
}
