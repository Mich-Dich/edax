#ifndef SHAPES_H
#define SHAPES_H
#include <stddef.h>
#include <stdint.h>

#define SHAPES_VERSION 0x0102
#define SHAPES_NAME "shapes"
#define SHAPES_PI 3.25
#define SHAPES_MAX(a, b) ((a) > (b) ? (a) : (b))
#define SHAPES_LIMIT (SHAPES_VERSION << 4)

typedef enum shape_kind { SHAPE_CIRCLE = 1, SHAPE_SQUARE, SHAPE_ANY = -1 } shape_kind;

typedef struct point {
    int32_t x;
    int32_t y;
} point;

struct shape {
    shape_kind kind;
    point      center;
    double     size;
    uint8_t    tag[4];
};

typedef union number {
    int64_t integer;
    double  real;
} number;

struct flags {
    unsigned visible : 1;
    unsigned solid : 1;
};

typedef struct registry registry;   /* opaque */
typedef int (*shape_visitor)(const struct shape* s, void* user);

size_t shapes_count(void);
double shape_area(const struct shape* s);
int shapes_visit(const struct shape* list, size_t n, shape_visitor visit, void* user);
registry* registry_make(void);
void registry_add(registry* r, struct shape s);
int registry_size(const registry* r);
void registry_free(registry* r);
number number_twice(number n, int is_real);
int shapes_scale(void);
int shapes_printf(const char* format, ...);
static inline int shapes_helper(int x) { return x + 1; }

#endif
