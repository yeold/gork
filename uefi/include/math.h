#ifndef _MATH_H
#define _MATH_H
double fabs(double x);
double floor(double x);
double sqrt(double x);
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define HUGE_VAL __builtin_huge_val()
#define NAN      __builtin_nan("")
#define INFINITY __builtin_inf()
#endif
