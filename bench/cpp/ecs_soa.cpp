// The workload of ecs.cpp with every component field in a column of its own, written by hand.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

struct soa3 { std::vector<float> x, y, z; };

template <class F>
static void each(std::vector<float>& a, const std::vector<float>& b, F system) {
    for (size_t i = 0; i < a.size(); ++i) system(a[i], b[i]);
}

template <class F>
static void each_one(std::vector<float>& a, F system) {
    for (size_t i = 0; i < a.size(); ++i) system(a[i]);
}

int main() {
    size_t n = 1000000;
    soa3 position, velocity;
    uint32_t seed = 12345;
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float r = (float)(seed >> 8) / 16777216.0f;
        position.x.push_back(r * 100.0f); position.y.push_back(r * 50.0f + 1.0f); position.z.push_back(r);
        velocity.x.push_back(r - 0.5f); velocity.y.push_back(0.0f); velocity.z.push_back(0.5f - r);
    }
    float dt = 0.01f;
    for (int frame = 0; frame < 200; ++frame) {
        each_one(velocity.y, [dt](float& vy) { vy -= 9.81f * dt; });
        each_one(velocity.x, [](float& v) { v *= 0.999f; });
        each_one(velocity.y, [](float& v) { v *= 0.999f; });
        each_one(velocity.z, [](float& v) { v *= 0.999f; });
        each(position.x, velocity.x, [dt](float& p, const float& v) { p += v * dt; });
        each(position.y, velocity.y, [dt](float& p, const float& v) { p += v * dt; });
        each(position.z, velocity.z, [dt](float& p, const float& v) { p += v * dt; });
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += (double)position.x[i] + (double)position.y[i] + (double)position.z[i];
    std::printf("%lld\n", (long long)std::llround(sum));
}
