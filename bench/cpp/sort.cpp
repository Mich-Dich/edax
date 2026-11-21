#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
    std::vector<uint32_t> v;
    uint32_t x = 2463534242u;
    for (uint32_t i = 0; i < 2000000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        v.push_back(x);
    }
    std::sort(v.begin(), v.end());
    for (size_t i = 1; i < v.size(); ++i) if (v[i - 1] > v[i]) { std::puts("not sorted"); return 1; }
    uint64_t check = (uint64_t)v[0] + v[v.size() / 2] + v[v.size() - 1];
    std::printf("%llu\n", (unsigned long long)check);
}
