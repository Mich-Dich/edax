#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
    std::vector<uint64_t> v;
    for (uint64_t i = 0; i < 20000000; ++i) v.push_back(i * 3);
    uint64_t sum = 0;
    for (size_t i = 0; i < v.size(); ++i) sum += v[i];
    for (uint64_t x : v) sum += x;
    std::printf("%llu\n", (unsigned long long)sum);
}
