/*
 * PS1 port — minimal C library shim (soft-float math).
 * The R3000A has no FPU, so these run entirely in software (via -msoft-float).
 */
#include "math.h"

#define PI_      3.14159265358979323846f
#define TWO_PI_  6.28318530717958647692f
#define HALF_PI_ 1.57079632679489661923f

float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

float fmodf(float x, float y) {
    if (y == 0.0f)
        return 0.0f;
    float q = x / y;
    float t = (float)(int)q; // truncate toward zero
    return x - t * y;
}

double fmod(double x, double y) {
    if (y == 0.0)
        return 0.0;
    double q = x / y;
    double t = (double)(long)q;
    return x - t * y;
}

/* Taylor series on [-pi/2, pi/2] (6 terms: error < 1e-7). fmodf alone leaves x in
 * (-2pi, 2pi), where the series diverges (it filled SinValue512/CosValue512 with garbage
 * past ~2.5 rad until phase 3c): fold to [-pi, pi], then sin(pi - x) = sin(x). */
float sinf(float x) {
    x = fmodf(x, TWO_PI_);
    if (x > PI_)
        x -= TWO_PI_;
    else if (x < -PI_)
        x += TWO_PI_;
    if (x > HALF_PI_)
        x = PI_ - x;
    else if (x < -HALF_PI_)
        x = -PI_ - x;
    float x2   = x * x;
    float term = x, sum = x;
    term *= -x2 / 6.0f;
    sum += term;
    term *= -x2 / 20.0f;
    sum += term;
    term *= -x2 / 42.0f;
    sum += term;
    term *= -x2 / 72.0f;
    sum += term;
    term *= -x2 / 110.0f;
    sum += term;
    return sum;
}

double sin(double x) {
    return (double)sinf((float)x);
}

float cosf(float x) {
    return sinf(x + HALF_PI_);
}

double cos(double x) {
    return (double)cosf((float)x);
}

float tanf(float x) {
    return sinf(x) / cosf(x);
}

double tan(double x) {
    return (double)tanf((float)x);
}

float sqrtf(float x) {
    if (x <= 0.0f)
        return 0.0f;
    float s = x;
    for (int i = 0; i < 24; ++i)
        s = 0.5f * (s + x / s);
    return s;
}

double sqrt(double x) {
    if (x <= 0.0)
        return 0.0;
    double s = x;
    for (int i = 0; i < 48; ++i)
        s = 0.5 * (s + x / s);
    return s;
}

float floorf(float x) {
    float t = (float)(int)x;
    return (x < t) ? t - 1.0f : t;
}

double floor(double x) {
    double t = (double)(long)x;
    return (x < t) ? t - 1.0 : t;
}

float ceilf(float x) {
    float t = (float)(int)x;
    return (x > t) ? t + 1.0f : t;
}

double ceil(double x) {
    double t = (double)(long)x;
    return (x > t) ? t + 1.0 : t;
}

float expf(float x) {
    // Taylor; adequate for |x| < ~10
    float sum = 1.0f, term = 1.0f;
    for (int i = 1; i <= 20; ++i) {
        term *= x / (float)i;
        sum += term;
    }
    return sum;
}

double exp(double x) {
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 30; ++i) {
        term *= x / (double)i;
        sum += term;
    }
    return sum;
}

float logf(float x) {
    if (x <= 0.0f)
        return 0.0f;
    // ln(x) = 2 * atanh((x-1)/(x+1)); converges for x > 0
    float t  = (x - 1.0f) / (x + 1.0f);
    float t2 = t * t;
    float sum = 0.0f, term = t;
    for (int i = 1; i < 60; i += 2) {
        sum += term / (float)i;
        term *= t2;
    }
    return 2.0f * sum;
}

double log(double x) {
    if (x <= 0.0)
        return 0.0;
    double t  = (x - 1.0) / (x + 1.0);
    double t2 = t * t;
    double sum = 0.0, term = t;
    for (int i = 1; i < 100; i += 2) {
        sum += term / (double)i;
        term *= t2;
    }
    return 2.0 * sum;
}

float powf(float base, float exp) {
    // fast path: non-negative integer exponent
    if (exp == (float)(int)exp && exp >= 0.0f && exp < 1000.0f) {
        float result = 1.0f;
        float b      = base;
        int n        = (int)exp;
        while (n) {
            if (n & 1)
                result *= b;
            b *= b;
            n >>= 1;
        }
        return result;
    }
    if (base <= 0.0f)
        return 0.0f;
    return expf(exp * logf(base));
}

double pow(double base, double exp) {
    return (double)powf((float)base, (float)exp);
}

float atan2f(float y, float x) {
    // quadrant-correct via atan(y/x) series is fiddly; use the sign/axis cases
    if (x == 0.0f) {
        if (y > 0.0f)
            return HALF_PI_;
        if (y < 0.0f)
            return -HALF_PI_;
        return 0.0f;
    }
    float at = 0.0f;
    // atan via series on the reduced argument
    float a = y / x;
    if (fabsf(a) < 1.0f) {
        float a2 = a * a;
        float t  = a;
        for (int i = 1; i <= 40; i += 2) {
            at += t / (float)i;
            t *= -a2;
        }
    } else {
        float ia = 1.0f / a;
        float i2 = ia * ia;
        float t  = ia;
        for (int i = 1; i <= 40; i += 2) {
            at += t / (float)i;
            t *= -i2;
        }
        at = HALF_PI_ - at;
    }
    if (x < 0.0f)
        at = PI_ - at;
    if (y < 0.0f)
        at = -at;
    return at;
}

double atan2(double y, double x) {
    return (double)atan2f((float)y, (float)x);
}
