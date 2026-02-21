// argv/argc smoke test.
// Exercises: new user-stack argv setup + CRT main(argc,argv).

#include <stdio.h>

int main(int argc, char** argv) {
    printf("argv_dump: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        const char* s = argv[i] ? argv[i] : "(null)";
        printf("argv_dump: argv[%d]=%s\n", i, s);
    }
    return 0;
}
