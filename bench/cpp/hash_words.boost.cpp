#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>
int main() {
    std::vector<std::string> words;
    for (uint32_t i = 0; i < 50000; ++i) words.push_back("word" + std::to_string(i * 7919));
    boost::unordered_flat_map<std::string, uint32_t> counts;
    uint32_t x = 2463534242u;
    for (uint32_t i = 0; i < 2000000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ++counts[words[x % 50000]];
    }
    std::printf("%zu %u\n", counts.size(), counts["word0"]);
}
