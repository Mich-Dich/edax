/* Structs passed by value in every register class of the C calling convention. */
#ifndef SHAPES_ABI_H
#define SHAPES_ABI_H
#include <stdint.h>

typedef struct { int32_t a, b; } pair_i32;          /* one INTEGER eightbyte */
typedef struct { float x, y, z; } vec3f;             /* SSE, SSE (float tail) */
typedef struct { double d; int32_t n; } mixed;       /* SSE, INTEGER */
typedef struct { int8_t a; int16_t b; int32_t c; } small_ints;
typedef struct { double m[3]; } big;                 /* 24 bytes: in memory */

pair_i32 abi_pair_swap(pair_i32 p);
vec3f abi_vec_scale(vec3f v, float k);
mixed abi_mixed_next(mixed m);
small_ints abi_small(small_ints s);
big abi_big_sum(big a, big b);
double abi_callback(double (*f)(big, vec3f));
/* Implemented in Rift (an exported extern function) and called from C. */
big rift_big_twice(big b);
double abi_call_rift(void);
#endif
