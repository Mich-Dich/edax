// The same scene update with GLM 1.0 (see edax/math_scene.ed). GLM has no shapes: the box
// transform and the frustum test are written out here the way std::math does them.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace glm;

struct box { vec3 min, max; };
struct plane { vec3 n; float d; };

static plane make_plane(float x, float y, float z, float w) {
    float r = 1.0f / std::sqrt(x * x + y * y + z * z);
    return {{x * r, y * r, z * r}, w * r};
}

static quat nlerp(const quat& a, const quat& b, float t) {
    float k = dot(a, b) < 0.0f ? -t : t;
    return normalize(a * (1.0f - t) + b * k);
}

int main() {
    size_t n = 20000;
    std::vector<vec3> positions, scales;
    std::vector<quat> from, to;
    std::vector<mat4> world(n, mat4(1.0f));
    std::vector<box> bounds(n);
    std::vector<unsigned char> visible(n);
    unsigned seed = 12345;
    std::vector<float> random;
    for (size_t i = 0; i < n * 14; ++i) {
        seed = seed * 1664525u + 1013904223u;
        random.push_back(float(seed >> 8) / 16777216.0f - 0.5f);
    }
    for (size_t i = 0; i < n; ++i) {
        size_t k = i * 14;
        positions.push_back(vec3(random[k], random[k + 1], random[k + 2]) * 6.0f);
        scales.push_back(vec3(random[k + 3] * 0.2f + 1.0f, random[k + 4] * 0.2f + 1.0f, random[k + 5] * 0.2f + 1.0f));
        from.push_back(normalize(quat(random[k + 9], random[k + 6], random[k + 7], random[k + 8])));
        to.push_back(normalize(quat(random[k + 13], random[k + 10], random[k + 11], random[k + 12])));
    }
    mat4 vp = perspectiveRH_ZO(radians(70.0f), 1.6f, 0.5f, 80.0f) * lookAtRH(vec3(0, 5, 30), vec3(0), vec3(0, 1, 0));
    plane planes[6] = {
        make_plane(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]),
        make_plane(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]),
        make_plane(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]),
        make_plane(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]),
        make_plane(vp[0][2], vp[1][2], vp[2][2], vp[3][2]),
        make_plane(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]),
    };
    size_t seen = 0;
    double sum = 0.0;
    auto local = [&](size_t i, float t) {
        return scale(translate(mat4(1.0f), positions[i]) * mat4_cast(nlerp(from[i], to[i], t)), scales[i]);
    };
    for (unsigned frame = 0; frame < 300; ++frame) {
        float t = float(frame) / 300.0f;
        world[0] = local(0, t);
        for (size_t i = 1; i < n; ++i) world[i] = world[(i - 1) / 4] * local(i, t);
        for (size_t i = 0; i < n; ++i) {
            const mat4& m = world[i];
            vec3 center = vec3(m[3]);
            vec3 extents = (abs(vec3(m[0])) + abs(vec3(m[1])) + abs(vec3(m[2]))) * 0.5f;
            bounds[i] = {center - extents, center + extents};
        }
        for (size_t i = 0; i < n; ++i) {
            vec3 c = (bounds[i].min + bounds[i].max) * 0.5f, e = (bounds[i].max - bounds[i].min) * 0.5f;
            bool inside = true;
            for (const plane& p : planes) {
                inside &= dot(p.n, c) + p.d + std::abs(p.n.x) * e.x + std::abs(p.n.y) * e.y + std::abs(p.n.z) * e.z >= 0.0f;
            }
            visible[i] = inside;
            seen += inside;
        }
        for (size_t i = 0; i < n; ++i) sum += double(world[i][3].x + world[i][3].y + world[i][3].z);
    }
    printf("%zu %lld\n", seen / 1000, (long long)(sum / 1000.0));
}
