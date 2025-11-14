#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* who = "world";
    if (argc > 1 && argv[1] && argv[1][0]) {
        who = argv[1];
    }
    write(1, "hello ", 6);
    write(1, who, (unsigned)strlen(who));
    write(1, "\n", 1);
    return 0;
}
