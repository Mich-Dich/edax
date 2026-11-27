// EnTT 3.13 for bench/edax/ecs_world.ed: the same phases with a registry and views.
#include <entt/entt.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct position { float x = 0, y = 0, z = 0; };
struct velocity { float x = 0, y = 0, z = 0; };
struct health { float value = 0; };
struct tag { std::uint32_t id = 0; };
struct marker { std::uint32_t id = 0; };

using clock_type = std::chrono::steady_clock;
static void phase(const char* name, clock_type::time_point start) {
    if (std::getenv("EDAX_BENCH_PHASES")) {
        std::fprintf(stderr, "  %s: %f ms\n", name, std::chrono::duration<double, std::milli>(clock_type::now() - start).count());
    }
}

int main() {
    const std::size_t n = 1000000;
    entt::registry w;
    std::vector<entt::entity> all;
    all.reserve(n);
    std::uint32_t seed = 12345;

    auto t0 = clock_type::now();
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float r = float(seed >> 8) / 16777216.0f;
        entt::entity e = w.create();
        w.emplace<position>(e, r * 100.0f, r * 50.0f, r);
        w.emplace<velocity>(e, r - 0.5f, 1.0f, 0.5f - r);
        all.push_back(e);
    }
    phase("spawn 1M (2 components)", t0);

    auto t1 = clock_type::now();
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 2 == 0) w.emplace<health>(all[i], 1.0f);
        if (i % 3 == 0) w.emplace<tag>(all[i], std::uint32_t(i));
    }
    phase("add health/tag to 833k", t1);

    const float dt = 0.01f;
    auto t2 = clock_type::now();
    for (int frame = 0; frame < 100; ++frame) {
        w.view<position, const velocity>().each([dt](position& p, const velocity& v) {
            p.x += v.x * dt;
            p.y += v.y * dt;
            p.z += v.z * dt;
        });
    }
    phase("100 x movement over 1M", t2);

    auto t3 = clock_type::now();
    for (int frame = 0; frame < 100; ++frame) {
        w.view<const position, health>().each([dt](const position& p, health& h) { h.value += p.y * dt * 0.001f; });
    }
    phase("100 x regen over 500k", t3);

    auto t4 = clock_type::now();
    for (std::size_t k = n - 1; k > 0; --k) {
        seed = seed * 1664525u + 1013904223u;
        std::size_t j = std::size_t(seed) % (k + 1);
        std::swap(all[k], all[j]);
    }
    double looked = 0;
    for (entt::entity e : all) looked += double(w.get<position>(e).x);
    phase("1M get<position> (shuffled)", t4);

    auto t5 = clock_type::now();
    for (std::size_t i = 0; i < 100000; ++i) w.emplace<marker>(all[i], 1u);
    for (std::size_t i = 0; i < 100000; ++i) w.remove<marker>(all[i]);
    phase("add+remove marker on 100k", t5);

    double sum = 0;
    w.view<const position, const velocity>().each([&sum](const position& p, const velocity& v) { sum += double(p.x + p.y + p.z + v.x); });
    w.view<const health>().each([&sum](const health& h) { sum += double(h.value); });

    auto t6 = clock_type::now();
    for (entt::entity e : all) w.destroy(e);
    phase("destroy 1M", t6);
    std::printf("%lld %lld %zu\n", (long long)sum, (long long)looked, std::size_t(w.storage<entt::entity>().free_list()));
    return 0;
}
