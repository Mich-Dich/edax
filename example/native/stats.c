/* A small C library that the ffi example links (built by build.ed as a C target). */
#include <stddef.h>
#include <stdint.h>

/* Implemented in Edax (ffi.ed) and exported to C. */
void edax_log(const char* message, int32_t value);

double stats_mean(const double* values, size_t count) {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += values[i];
    edax_log("stats_mean summed values", (int32_t)count);
    return sum / (double)count;
}

/* Sorts in place (insertion sort) and returns the number of swaps. */
int32_t stats_sort(int32_t* values, size_t count) {
    int32_t swaps = 0;
    for (size_t i = 1; i < count; ++i) {
        for (size_t j = i; j > 0 && values[j - 1] > values[j]; --j) {
            int32_t t = values[j];
            values[j] = values[j - 1];
            values[j - 1] = t;
            ++swaps;
        }
    }
    return swaps;
}
