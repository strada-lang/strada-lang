/* c_shared_lib.h - C-callable interface for Strada mathlib */
#ifndef C_SHARED_LIB_H
#define C_SHARED_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

/* C-callable wrapper functions */
int c_add_integers(int a, int b);
int c_multiply_integers(int a, int b);
int c_subtract_integers(int a, int b);

#ifdef __cplusplus
}
#endif

#endif /* C_SHARED_LIB_H */
