#include <cstdint>
#include <cstdio>
#include <absl/container/flat_hash_map.h>
int main() {
    absl::flat_hash_map<uint64_t, uint64_t> m;
    uint64_t x = 88172645463325252ull;
    for (uint64_t i = 0; i < 2000000; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; m[x] = i; }
    uint64_t found = 0, y = 88172645463325252ull;
    for (uint64_t i = 0; i < 2000000; ++i) {
        y ^= y << 13; y ^= y >> 7; y ^= y << 17;
        found += m.count(y);
        found += m.count(y + 1);
    }
    uint64_t z = 88172645463325252ull;
    for (uint64_t i = 0; i < 1000000; ++i) { z ^= z << 13; z ^= z >> 7; z ^= z << 17; m.erase(z); }
    std::printf("%zu %llu\n", m.size(), (unsigned long long)found);
}
