// The same reduction as std::ui::plot::decimate_indexed: points in [low, high] (and one neighbour
// on each side), at most first/lowest/highest/last per column once there are more than 4 per column.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static void emit(std::vector<double>& out, const std::vector<float>& ys, size_t first, size_t lo, size_t hi, size_t last) {
    size_t a = lo < hi ? lo : hi, b = lo < hi ? hi : lo;
    size_t order[4] = {first, a, b, last};
    size_t prev = 0;
    for (int k = 0; k < 4; ++k) {
        size_t i = order[k];
        if (k > 0 && i == prev) continue;
        out.push_back(double(i));
        out.push_back(double(ys[i]));
        prev = i;
    }
}

static size_t decimate(const std::vector<float>& ys, double low, double high, unsigned columns, std::vector<double>& out) {
    size_t n = ys.size(), before = out.size();
    if (n == 0) return 0;
    double width = high > low ? (high - low) / double(columns) : 1.0;
    size_t in_range = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = double(i);
        if (x >= low && x <= high) ++in_range;
    }
    bool reduce = in_range > 4 * size_t(columns);
    bool have = false;
    int64_t current = 0;
    size_t first = 0, lo = 0, hi = 0, last = 0;
    double lo_y = 0, hi_y = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = double(i), y = double(ys[i]);
        if (x < low) {
            if (i + 1 < n && double(i + 1) >= low) { out.push_back(x); out.push_back(y); }
            continue;
        }
        if (x > high) {
            if (have) { emit(out, ys, first, lo, hi, last); have = false; }
            out.push_back(x); out.push_back(y);
            break;
        }
        if (!reduce) { out.push_back(x); out.push_back(y); continue; }
        int64_t c = int64_t(std::floor((x - low) / width));
        if (!have || c != current) {
            if (have) emit(out, ys, first, lo, hi, last);
            have = true; current = c; first = lo = hi = i; lo_y = hi_y = y;
        }
        if (y < lo_y) { lo_y = y; lo = i; }
        if (y > hi_y) { hi_y = y; hi = i; }
        last = i;
    }
    if (have) emit(out, ys, first, lo, hi, last);
    return (out.size() - before) / 2;
}

int main() {
    std::vector<float> ys;
    uint32_t x = 2463534242u;
    for (uint32_t i = 0; i < 1000000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ys.push_back(float(x % 10000) * 0.001f);
    }
    std::vector<double> out;
    out.reserve(8000);
    size_t points = 0;
    double sum = 0;
    for (uint32_t round = 0; round < 200; ++round) {
        out.clear();
        double low = double(round);
        size_t n = decimate(ys, low, low + 999000.0, 1000, out);
        points += n;
        sum += out[2 * (n / 2) + 1];
    }
    std::printf("%zu %lld\n", points, (long long)(sum * 1000.0));
}
