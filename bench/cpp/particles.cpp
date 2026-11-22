#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
static void step(std::vector<float>& px, std::vector<float>& py, std::vector<float>& vx, std::vector<float>& vy, float dt) {
    for (size_t i = 0; i < px.size(); ++i) {
        vy[i] -= 9.81f * dt;
        vx[i] *= 0.999f;
        vy[i] *= 0.999f;
        px[i] += vx[i] * dt;
        py[i] += vy[i] * dt;
        if (py[i] < 0.0f) {
            py[i] = -py[i];
            vy[i] = -vy[i] * 0.8f;
        }
    }
}
int main() {
    size_t n = 200000;
    std::vector<float> px, py, vx, vy;
    uint32_t seed = 12345;
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float r = (float)(seed >> 8) / 16777216.0f;
        px.push_back(r * 100.0f);
        py.push_back(r * 50.0f + 1.0f);
        vx.push_back(r - 0.5f);
        vy.push_back(0.0f);
    }
    for (int s = 0; s < 1000; ++s) step(px, py, vx, vy, 0.01f);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += (double)px[i] + (double)py[i];
    std::printf("%llu\n", (unsigned long long)std::llround(sum));
}
