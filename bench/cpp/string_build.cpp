#include <cstdio>
#include <string>
int main() {
    std::string s;
    for (unsigned i = 0; i < 5000000; ++i) { s += "ab"; s += 'c'; }
    size_t total = 0;
    for (unsigned i = 0; i < 2000000; ++i) {
        std::string t = "key";
        t += 'x';
        total += t.size();
    }
    std::printf("%zu %zu\n", s.size(), total);
}
