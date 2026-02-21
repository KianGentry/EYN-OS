#pragma once

// Minimal stdnoreturn.h

#ifdef __chibicc__
#define noreturn
#else
#define noreturn _Noreturn
#endif
