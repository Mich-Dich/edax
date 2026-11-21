#include <shapes/shapes.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

struct registry { int n; };

size_t shapes_count(void) { return 2; }
double shape_area(const struct shape* s) { return s->kind == SHAPE_SQUARE ? s->size * s->size : 3.0 * s->size * s->size; }
int shapes_visit(const struct shape* list, size_t n, shape_visitor visit, void* user) {
    int total = 0;
    for (size_t i = 0; i < n; ++i) total += visit(&list[i], user);
    return total;
}
registry* registry_make(void) { registry* r = malloc(sizeof *r); r->n = 0; return r; }
void registry_add(registry* r, struct shape s) { r->n += s.tag[0]; }
int registry_size(const registry* r) { return r->n; }
void registry_free(registry* r) { free(r); }
number number_twice(number n, int is_real) { if (is_real) n.real *= 2; else n.integer *= 2; return n; }
int shapes_scale(void) { return SHAPES_SCALE; }
int shapes_printf(const char* format, ...) { va_list a; va_start(a, format); int r = vprintf(format, a); va_end(a); return r; }

#include <shapes/abi.h>
pair_i32 abi_pair_swap(pair_i32 p) { pair_i32 r = {p.b, p.a}; return r; }
vec3f abi_vec_scale(vec3f v, float k) { vec3f r = {v.x * k, v.y * k, v.z * k}; return r; }
mixed abi_mixed_next(mixed m) { mixed r = {m.d + 0.5, m.n + 1}; return r; }
small_ints abi_small(small_ints s) { small_ints r = {(int8_t)(s.a + 1), (int16_t)(s.b * 2), s.c * 3}; return r; }
big abi_big_sum(big a, big b) { big r; for (int i = 0; i < 3; ++i) r.m[i] = a.m[i] + b.m[i]; return r; }
double abi_call_rift(void) { big b = {{1.0, 2.0, 3.0}}; big r = rift_big_twice(b); return r.m[0] + r.m[1] + r.m[2]; }
