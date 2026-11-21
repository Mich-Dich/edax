#include <cstdint>
#include <cstdio>
#include <absl/container/btree_map.h>
int main() {
    absl::btree_map<uint32_t, uint32_t> m;
    uint32_t x = 2463534242u;
    for (uint32_t i = 0; i < 1000000; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; m[x] = i; }
    uint32_t found = 0, y = 2463534242u;
    for (uint32_t i = 0; i < 1000000; ++i) {
        y ^= y << 13; y ^= y >> 17; y ^= y << 5;
        found += m.count(y); found += m.count(y + 1);
    }
    uint64_t walk = 0; uint32_t previous = 0; bool sorted = true;
    for (auto& [k, v] : m) { if (k < previous) sorted = false; previous = k; walk += v; }
    std::printf("%zu %u %llu %s\n", m.size(), found, (unsigned long long)walk, sorted ? "true" : "false");
}
