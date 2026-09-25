/*
 * PS1 port — minimal C library shim (math.h)
 *
 * Soft-float implementations of the handful of functions RSDKv2 uses.
 * sinf/cosf feed CalculateTrigAngles(); pow() is used once (pow(10, i)).
 * Accuracy is sufficient for 2D game logic; these are not high-precision.
 */
#ifndef PS1_MATH_H
#define PS1_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923

float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  sqrtf(float x);
float  fabsf(float x);
float  floorf(float x);
float  ceilf(float x);
float  fmodf(float x, float y);
float  powf(float base, float exp);
float  logf(float x);
float  expf(float x);
float  atan2f(float y, float x);

double sin(double x);
double cos(double x);
double tan(double x);
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double pow(double base, double exp);
double log(double x);
double exp(double x);
double atan2(double y, double x);

#ifdef __cplusplus
}
#endif

#endif /* PS1_MATH_H */
