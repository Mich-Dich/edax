#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
struct record { uint32_t key, id; };
int main() {
    std::vector<record> v;
    uint32_t x = 2463534242u;
    for (uint32_t i = 0; i < 500000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        v.push_back({x % 1000, i});
    }
    std::stable_sort(v.begin(), v.end(), [](const record& a, const record& b) { return a.key < b.key; });
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i-1].key > v[i].key || (v[i-1].key == v[i].key && v[i-1].id > v[i].id)) { std::puts("bad"); return 1; }
    std::printf("%u %u\n", v[0].id, v[v.size() - 1].id);
}
