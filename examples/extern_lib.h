/* extern_lib.h - C-callable Strada functions */
#ifndef EXTERN_LIB_H
#define EXTERN_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

int add_integers(int a, int b);
int multiply_integers(int a, int b);
int subtract_integers(int a, int b);
double add_floats(double a, double b);

#ifdef __cplusplus
}
#endif

#endif /* EXTERN_LIB_H */
