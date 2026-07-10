/*
 * math.c -- Software implementations of math functions for chibicc.
 *
 * These are portable C implementations used when the compiler does not
 * support inline assembly (chibicc).  They are linked into libeync.a so
 * any userland program compiled with chibicc can use <math.h>.
 *
 * Accuracy: sufficient for game-quality computations (DOOM lookup-table
 * initialisation, GUI rendering, etc.).  Not bit-exact with x87 results.
 *
 * The GCC build path uses x87 inline asm via static-inline functions in
 * math.h and does NOT link this file.
 */

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Absolute value                                                     */
/* ------------------------------------------------------------------ */

double fabs(double x) { return x < 0.0 ? -x : x; }
float  fabsf(float x) { return x < 0.0f ? -x : x; }

/* ------------------------------------------------------------------ */
/*  Floor / ceil / trunc / round                                       */
/* ------------------------------------------------------------------ */

double trunc(double x) {
    /* Cast to long truncates toward zero. */
    if (x >= 0.0) {
        if (x > 2147483647.0) return x; /* overflow guard */
        return (double)(long)x;
    } else {
        if (x < -2147483648.0) return x;
        return (double)(long)x;
    }
}

double floor(double x) {
    double t = trunc(x);
    if (t > x) return t - 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (t < x) return t + 1.0;
    return t;
}

double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

float  floorf(float x) { return (float)floor((double)x); }
float  ceilf(float x)  { return (float)ceil((double)x); }
float  roundf(float x) { return (float)round((double)x); }
float  truncf(float x) { return (float)trunc((double)x); }

/* ------------------------------------------------------------------ */
/*  fmod                                                               */
/* ------------------------------------------------------------------ */

double fmod(double x, double y) {
    if (y == 0.0) return x;
    return x - trunc(x / y) * y;
}

float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

/* ------------------------------------------------------------------ */
/*  Square root (Newton–Raphson)                                       */
/* ------------------------------------------------------------------ */

double sqrt(double x) {
    if (x < 0.0) return 0.0;
    if (x == 0.0) return 0.0;

    /* Initial guess via integer bit manipulation of the exponent. */
    double guess = x;
    /* Newton iterations: x_{n+1} = (x_n + S/x_n) / 2 */
    for (int i = 0; i < 20; i++)
        guess = 0.5 * (guess + x / guess);
    return guess;
}

float sqrtf(float x) { return (float)sqrt((double)x); }

/* ------------------------------------------------------------------ */
/*  Trigonometric functions (minimax polynomial / range reduction)      */
/* ------------------------------------------------------------------ */

/*
 * Reduce x to [-pi, pi] range for sin/cos.
 */
static double reduce_angle(double x) {
    /* x mod (2*pi) */
    double twopi = 2.0 * M_PI;
    x = fmod(x, twopi);
    if (x > M_PI) x -= twopi;
    if (x < -M_PI) x += twopi;
    return x;
}

/*
 * sin(x) -- Taylor series around 0, reduced range.
 * sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ...
 */
double sin(double x) {
    x = reduce_angle(x);
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int i = 1; i <= 10; i++) {
        term *= -x2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

/*
 * cos(x) -- Taylor series around 0, reduced range.
 * cos(x) = 1 - x^2/2! + x^4/4! - x^6/6! + ...
 */
double cos(double x) {
    x = reduce_angle(x);
    double x2 = x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 10; i++) {
        term *= -x2 / (double)((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return 1e308; /* near-infinity */
    return sin(x) / c;
}

/*
 * atan(x) -- Maclaurin series with range reduction.
 *
 * For |x| <= 1:  atan(x) = x - x^3/3 + x^5/5 - x^7/7 + …
 * For |x| > 1:   atan(x) = pi/2 - atan(1/x)
 */
double atan(double x) {
    int neg = 0;
    if (x < 0.0) { neg = 1; x = -x; }

    int recip = 0;
    if (x > 1.0) { recip = 1; x = 1.0 / x; }

    /* Reduce further: if x > tan(pi/12) ≈ 0.2679, use
     * atan(x) = pi/6 + atan((x - 1/sqrt(3)) / (1 + x/sqrt(3))) */
    double result;
    double sqrt3 = 1.7320508075688772;
    if (x > 0.2679491924311228) {
        double xp = (x * sqrt3 - 1.0) / (sqrt3 + x);
        double xp2 = xp * xp;
        double term = xp;
        double sum = xp;
        for (int i = 1; i <= 15; i++) {
            term *= -xp2;
            sum += term / (double)(2 * i + 1);
        }
        result = M_PI / 6.0 + sum;
    } else {
        double x2 = x * x;
        double term = x;
        double sum = x;
        for (int i = 1; i <= 15; i++) {
            term *= -x2;
            sum += term / (double)(2 * i + 1);
        }
        result = sum;
    }

    if (recip) result = M_PI_2 - result;
    if (neg) result = -result;
    return result;
}

double atan2(double y, double x) {
    if (x > 0.0)           return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + M_PI;
    if (x < 0.0 && y < 0.0)  return atan(y / x) - M_PI;
    if (x == 0.0 && y > 0.0) return M_PI_2;
    if (x == 0.0 && y < 0.0) return -M_PI_2;
    return 0.0; /* x == 0, y == 0: undefined */
}

double asin(double x) { return atan2(x, sqrt(1.0 - x * x)); }
double acos(double x) { return atan2(sqrt(1.0 - x * x), x); }

float sinf(float x)   { return (float)sin((double)x); }
float cosf(float x)   { return (float)cos((double)x); }
float tanf(float x)   { return (float)tan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float atanf(float x)  { return (float)atan((double)x); }
float asinf(float x)  { return (float)asin((double)x); }
float acosf(float x)  { return (float)acos((double)x); }

/* ------------------------------------------------------------------ */
/*  Exponential / logarithmic                                          */
/* ------------------------------------------------------------------ */

/*
 * exp(x) -- Taylor series: e^x = 1 + x + x^2/2! + x^3/3! + …
 * Argument reduction: exp(x) = exp(n*ln2 + r) = 2^n * exp(r)
 * where r = x - n*ln2 and |r| < ln2/2.
 */
double exp(double x) {
    if (x > 709.0) return 1e308;
    if (x < -709.0) return 0.0;

    /* Split: n = round(x/ln2), r = x - n*ln2 */
    double n = round(x / M_LN2);
    double r = x - n * M_LN2;

    /* Taylor for exp(r), |r| < 0.347 */
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 20; i++) {
        term *= r / (double)i;
        sum += term;
    }

    /* Multiply by 2^n */
    int ni = (int)n;
    if (ni > 0) { for (int i = 0; i < ni; i++) sum *= 2.0; }
    else if (ni < 0) { for (int i = 0; i < -ni; i++) sum *= 0.5; }

    return sum;
}

/*
 * log(x) -- natural logarithm.
 * Reduce: x = m * 2^e where 1 <= m < 2.
 * Then ln(x) = e*ln(2) + ln(m).
 * ln(m) via series: ln(1+t) = t - t^2/2 + t^3/3 - … where t = m - 1.
 */
double log(double x) {
    if (x <= 0.0) return -1e308;
    if (x == 1.0) return 0.0;

    int e = 0;
    double m = x;
    while (m >= 2.0) { m *= 0.5; e++; }
    while (m < 1.0)  { m *= 2.0; e--; }

    /* m is now in [1, 2), compute ln(m) via series for ln(1+t), t = m-1 */
    double t = m - 1.0;
    double term = t;
    double sum = t;
    for (int i = 2; i <= 30; i++) {
        term *= -t;
        sum += term / (double)i;
    }

    return (double)e * M_LN2 + sum;
}

double log2(double x)  { return log(x) / M_LN2; }
double log10(double x) { return log(x) / M_LN10; }

double pow(double base, double exponent) {
    if (base == 0.0) return 0.0;
    if (exponent == 0.0) return 1.0;
    /* Check for integer exponent for better accuracy */
    if (exponent == (double)(int)exponent && exponent > 0.0 && exponent <= 30.0) {
        double r = 1.0;
        int n = (int)exponent;
        for (int i = 0; i < n; i++) r *= base;
        return r;
    }
    return exp(exponent * log(base));
}

float log2f(float x)          { return (float)log2((double)x); }
float logf(float x)           { return (float)log((double)x); }
float log10f(float x)         { return (float)log10((double)x); }
float expf(float x)           { return (float)exp((double)x); }
float powf(float b, float e)  { return (float)pow((double)b, (double)e); }

/* ------------------------------------------------------------------ */
/*  Hyperbolic                                                         */
/* ------------------------------------------------------------------ */

double sinh(double x) { double e = exp(x); return (e - 1.0/e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0/e) * 0.5; }
double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

/* ------------------------------------------------------------------ */
/*  Min / max                                                          */
/* ------------------------------------------------------------------ */

double fmin(double x, double y) { return x < y ? x : y; }
double fmax(double x, double y) { return x > y ? x : y; }
float  fminf(float x, float y) { return x < y ? x : y; }
float  fmaxf(float x, float y) { return x > y ? x : y; }

/* ------------------------------------------------------------------ */
/*  Classification                                                     */
/* ------------------------------------------------------------------ */

int isnan(double x)    { return x != x; }
int isinf(double x)    { return !isnan(x) && isnan(x - x); }
int isfinite(double x) { return !isnan(x) && !isinf(x); }
