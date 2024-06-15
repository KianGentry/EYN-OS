#include "print.h"

void kernel_main()
{
    print_clear();
    print_set_color(P_LB, P_B);
    print_str("THIS IS EYN-OS!!!!!");
}
