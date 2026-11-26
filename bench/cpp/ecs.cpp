// The same workload as edax/ecs.ed: component columns as std::vector, systems as lambdas called
// per entity by a generic each (the shape of EnTT's view.each).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

struct vec3 { float x = 0, y = 0, z = 0; };

template <class A, class B, class F>
static void each(std::vector<A>& a, const std::vector<B>& b, F system) {
    for (size_t i = 0; i < a.size(); ++i) system(a[i], b[i]);
}

template <class A, class F>
static void each_one(std::vector<A>& a, F system) {
    for (size_t i = 0; i < a.size(); ++i) system(a[i]);
}

int main() {
    size_t n = 1000000;
    std::vector<vec3> position, velocity;
    uint32_t seed = 12345;
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float r = (float)(seed >> 8) / 16777216.0f;
        position.push_back({r * 100.0f, r * 50.0f + 1.0f, r});
        velocity.push_back({r - 0.5f, 0.0f, 0.5f - r});
    }
    float dt = 0.01f;
    for (int frame = 0; frame < 200; ++frame) {
        each_one(velocity, [dt](vec3& v) { v.y -= 9.81f * dt; });
        each_one(velocity, [](vec3& v) { v.x *= 0.999f; v.y *= 0.999f; v.z *= 0.999f; });
        each(position, velocity, [dt](vec3& p, const vec3& v) {
            p.x += v.x * dt;
            p.y += v.y * dt;
            p.z += v.z * dt;
        });
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += (double)position[i].x + (double)position[i].y + (double)position[i].z;
    std::printf("%lld\n", (long long)std::llround(sum));
}
