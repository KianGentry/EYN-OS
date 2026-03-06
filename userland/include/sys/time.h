#pragma once

/*
 * sys/time.h compatibility stub for EYN-OS userland.
 *
 * DOOM (and other POSIX programs) include <sys/time.h> for struct timeval
 * and gettimeofday().  On EYN-OS these are defined in <time.h>; this header
 * simply pulls them in so both include paths work.
 */

#include <time.h>
