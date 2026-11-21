#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
static std::vector<std::string_view> split(std::string_view t, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i < t.size(); ++i) if (t[i] == sep) { out.push_back(t.substr(start, i - start)); start = i + 1; }
    out.push_back(t.substr(start));
    return out;
}
static uint64_t parse(std::string_view s) { uint64_t v = 0; std::from_chars(s.data(), s.data() + s.size(), v); return v; }
int main() {
    std::string text;
    char buf[64];
    for (uint32_t i = 0; i < 1000000; ++i) { int n = std::snprintf(buf, sizeof buf, "%u,%u,name%u\n", i, i % 97, i % 13); text.append(buf, n); }
    uint64_t sum = 0; size_t lines = 0;
    for (std::string_view line : split(text, '\n')) {
        if (line.empty()) continue;
        auto parts = split(line, ',');
        sum += parse(parts[0]) + parse(parts[1]);
        if (parts[2].starts_with("name1")) ++lines;
    }
    std::printf("%llu %zu\n", (unsigned long long)sum, lines);
}
