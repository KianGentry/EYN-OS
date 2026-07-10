/*
 * math.h -- Minimal math library for EYN-OS userland programs.
 *
 * When compiled with GCC/i686-elf-gcc, functions are implemented as
 * static inline using x87 FPU instructions (the kernel initialises the
 * FPU during boot -- see src/cpu/fpu.c).
 *
 * When compiled with chibicc (__chibicc__ defined), inline assembly is
 * unavailable.  Functions are declared as external and implemented in
 * userland/libc/math.c using portable C (Taylor series, Newton–Raphson,
 * etc.).  The implementations are linked via libeync.a.
 *
 * ABI-INVARIANT: Function signatures match ISO C90/C99 <math.h>.
 * Only double-precision variants are provided; float/long-double
 * wrappers are trivially defined below.
 */
#ifndef _MATH_H
#define _MATH_H

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

#ifdef __chibicc__
/* chibicc does not support __builtin_huge_val etc.; use large literal. */
#define HUGE_VAL    1e308
#define INFINITY    (1e308 * 1e308)
#define NAN         (0.0 / 0.0)
#else
#define HUGE_VAL    (__builtin_huge_val())
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))
#endif

#ifdef __chibicc__
/* ------------------------------------------------------------------ */
/*  chibicc path: external declarations (linked from math.c)           */
/* ------------------------------------------------------------------ */
double fabs(double x);
float  fabsf(float x);
double sqrt(double x);
float  sqrtf(float x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double atan(double x);
double asin(double x);
double acos(double x);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  atan2f(float y, float x);
float  atanf(float x);
float  asinf(float x);
float  acosf(float x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
float  floorf(float x);
float  ceilf(float x);
float  roundf(float x);
float  truncf(float x);
double fmod(double x, double y);
float  fmodf(float x, float y);
double log2(double x);
double log(double x);
double log10(double x);
double exp(double x);
double pow(double base, double exponent);
float  log2f(float x);
float  logf(float x);
float  log10f(float x);
float  expf(float x);
float  powf(float b, float e);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double fmin(double x, double y);
double fmax(double x, double y);
float  fminf(float x, float y);
float  fmaxf(float x, float y);
int    isnan(double x);
int    isinf(double x);
int    isfinite(double x);

#else /* GCC / inline-asm path */

/* ------------------------------------------------------------------ */
/*  Absolute value                                                     */
/* ------------------------------------------------------------------ */

static inline double fabs(double x) {
    double r;
    __asm__ __volatile__("fabs" : "=t"(r) : "0"(x));
    return r;
}

static inline float fabsf(float x) { return (float)fabs((double)x); }

/* ------------------------------------------------------------------ */
/*  Square root                                                        */
/* ------------------------------------------------------------------ */

static inline double sqrt(double x) {
    double r;
    __asm__ __volatile__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

static inline float sqrtf(float x) { return (float)sqrt((double)x); }

/* ------------------------------------------------------------------ */
/*  Trigonometric functions                                            */
/* ------------------------------------------------------------------ */

static inline double sin(double x) {
    double r;
    __asm__ __volatile__("fsin" : "=t"(r) : "0"(x));
    return r;
}

static inline double cos(double x) {
    double r;
    __asm__ __volatile__("fcos" : "=t"(r) : "0"(x));
    return r;
}

/*
 * atan2(y, x) -- implemented via fpatan which computes arctan(y/x)
 * and returns the result in the correct quadrant.
 *
 * x87 fpatan: ST(1) = atan2(ST(1), ST(0)); pop ST(0)
 * GCC inline asm: "u" constraint = ST(1), "t" constraint = ST(0).
 * Result left in ST(0) after pop.
 */
static inline double atan2(double y, double x) {
    double r;
    __asm__ __volatile__(
        "fpatan"
        : "=t"(r)
        : "0"(x), "u"(y)
        : "st(1)"
    );
    return r;
}

static inline double atan(double x) {
    return atan2(x, 1.0);
}

static inline double tan(double x) {
    /*
     * fptan pushes 1.0 onto the stack and leaves tan(x) in ST(1).
     * We pop both to get the tangent value.
     */
    double r, one;
    __asm__ __volatile__(
        "fptan"
        : "=t"(one), "=u"(r)
        : "0"(x)
    );
    (void)one;
    return r;
}

static inline double asin(double x) {
    /* asin(x) = atan2(x, sqrt(1 - x*x)) */
    return atan2(x, sqrt(1.0 - x * x));
}

static inline double acos(double x) {
    /* acos(x) = atan2(sqrt(1 - x*x), x) */
    return atan2(sqrt(1.0 - x * x), x);
}

static inline float sinf(float x) { return (float)sin((double)x); }
static inline float cosf(float x) { return (float)cos((double)x); }
static inline float tanf(float x) { return (float)tan((double)x); }
static inline float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
static inline float atanf(float x) { return (float)atan((double)x); }
static inline float asinf(float x) { return (float)asin((double)x); }
static inline float acosf(float x) { return (float)acos((double)x); }

/* ------------------------------------------------------------------ */
/*  Rounding                                                           */
/* ------------------------------------------------------------------ */

static inline double floor(double x) {
    double r;
    short old_cw, new_cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(old_cw));
    new_cw = (old_cw & ~0x0C00) | 0x0400; /* Round toward -infinity */
    __asm__ __volatile__("fldcw %0" : : "m"(new_cw));
    __asm__ __volatile__("frndint" : "=t"(r) : "0"(x));
    __asm__ __volatile__("fldcw %0" : : "m"(old_cw));
    return r;
}

static inline double ceil(double x) {
    double r;
    short old_cw, new_cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(old_cw));
    new_cw = (old_cw & ~0x0C00) | 0x0800; /* Round toward +infinity */
    __asm__ __volatile__("fldcw %0" : : "m"(new_cw));
    __asm__ __volatile__("frndint" : "=t"(r) : "0"(x));
    __asm__ __volatile__("fldcw %0" : : "m"(old_cw));
    return r;
}

static inline double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

static inline double trunc(double x) {
    double r;
    short old_cw, new_cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(old_cw));
    new_cw = (old_cw & ~0x0C00) | 0x0C00; /* Round toward zero */
    __asm__ __volatile__("fldcw %0" : : "m"(new_cw));
    __asm__ __volatile__("frndint" : "=t"(r) : "0"(x));
    __asm__ __volatile__("fldcw %0" : : "m"(old_cw));
    return r;
}

static inline float floorf(float x) { return (float)floor((double)x); }
static inline float ceilf(float x) { return (float)ceil((double)x); }
static inline float roundf(float x) { return (float)round((double)x); }
static inline float truncf(float x) { return (float)trunc((double)x); }

/* ------------------------------------------------------------------ */
/*  Remainder / modulus                                                */
/* ------------------------------------------------------------------ */

static inline double fmod(double x, double y) {
    if (y == 0.0) return x;
    return x - trunc(x / y) * y;
}

static inline float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

/* ------------------------------------------------------------------ */
/*  Exponential / logarithmic                                          */
/* ------------------------------------------------------------------ */

/*
 * fyl2x: ST(1) * log2(ST(0)) → leaves result in ST(0), pops ST(1).
 * log2(x) = 1.0 * log2(x)
 * log(x)  = log2(x) * ln(2)
 * log10(x) = log2(x) * log10(2)
 */
static inline double log2(double x) {
    double r;
    __asm__ __volatile__(
        "fld1\n\t"
        "fxch\n\t"
        "fyl2x"
        : "=t"(r)
        : "0"(x)
    );
    return r;
}

static inline double log(double x) {
    return log2(x) * M_LN2;
}

static inline double log10(double x) {
    return log2(x) * 0.30102999566398119521;
}

/*
 * exp(x) = 2^(x / ln(2))
 * f2xm1 computes 2^x - 1 for |x| <= 1.  For larger values we split
 * into integer + fractional parts.
 */
static inline double exp(double x) {
    double val = x / M_LN2;  /* Convert to base-2 exponent */
    double int_part = trunc(val);
    double frac = val - int_part;
    double frac_result;
    __asm__ __volatile__(
        "f2xm1"
        : "=t"(frac_result)
        : "0"(frac)
    );
    frac_result += 1.0;
    /* Scale by 2^int_part using fscale */
    double result;
    __asm__ __volatile__(
        "fscale"
        : "=t"(result)
        : "0"(frac_result), "u"(int_part)
    );
    return result;
}

static inline double pow(double base, double exponent) {
    if (base == 0.0) return 0.0;
    if (exponent == 0.0) return 1.0;
    return exp(exponent * log(base));
}

static inline float log2f(float x) { return (float)log2((double)x); }
static inline float logf(float x) { return (float)log((double)x); }
static inline float log10f(float x) { return (float)log10((double)x); }
static inline float expf(float x) { return (float)exp((double)x); }
static inline float powf(float b, float e) { return (float)pow((double)b, (double)e); }

/* ------------------------------------------------------------------ */
/*  Hyperbolic (derived from exp/log)                                  */
/* ------------------------------------------------------------------ */

static inline double sinh(double x) { double e = exp(x); return (e - 1.0/e) * 0.5; }
static inline double cosh(double x) { double e = exp(x); return (e + 1.0/e) * 0.5; }
static inline double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

/* ------------------------------------------------------------------ */
/*  Min / max                                                          */
/* ------------------------------------------------------------------ */

static inline double fmin(double x, double y) { return x < y ? x : y; }
static inline double fmax(double x, double y) { return x > y ? x : y; }
static inline float fminf(float x, float y) { return x < y ? x : y; }
static inline float fmaxf(float x, float y) { return x > y ? x : y; }

/* ------------------------------------------------------------------ */
/*  Classification (minimal)                                           */
/* ------------------------------------------------------------------ */

static inline int isnan(double x) { return x != x; }
static inline int isinf(double x) { return !isnan(x) && isnan(x - x); }
static inline int isfinite(double x) { return !isnan(x) && !isinf(x); }

#endif /* !__chibicc__ (GCC inline-asm path) */

#endif /* _MATH_H */
