/*
 * values.h compatibility stub for EYN-OS.
 *
 * The original Linux doomtype.h includes <values.h> to get MAXCHAR, MAXSHORT,
 * MAXINT, MAXLONG and their MIN* counterparts.  BSD/Linux's values.h ultimately
 * defines these via <limits.h>.  We define them directly here to avoid pulling
 * in any host system headers.
 *
 * These values match the 32-bit two's-complement representation that DOOM
 * was written for and that EYN-OS's freestanding GCC target uses.
 */
#pragma once
#ifndef _VALUES_H
#define _VALUES_H

#define MAXCHAR   ((char)0x7f)
#define MAXSHORT  ((short)0x7fff)
#define MAXINT    ((int)0x7fffffff)
#define MAXLONG   ((long)0x7fffffff)
#define MINCHAR   ((char)0x80)
#define MINSHORT  ((short)0x8000)
#define MININT    ((int)0x80000000)
#define MINLONG   ((long)0x80000000)

#endif /* _VALUES_H */
