/*
 * malloc.h compatibility stub for EYN-OS.
 *
 * On Linux, <malloc.h> was historically the home of malloc() etc.  These
 * functions now live in <stdlib.h>.  This stub simply re-exports <stdlib.h>
 * so that source files that include <malloc.h> (e.g., DOOM's w_wad.c) compile
 * without modification.
 */
#pragma once
#ifndef _MALLOC_H
#define _MALLOC_H

#include <stdlib.h>

#endif /* _MALLOC_H */
