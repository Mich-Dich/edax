#version 330 core
in vec2 v_uv;
in vec4 v_color;
in vec4 v_local_half;
in vec4 v_shape;
uniform sampler2D image;
out vec4 result;
float box(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - half_size + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    vec4 c = v_color;
    float kind = v_shape.w;
    if (kind < 0.5) {
        c *= texture(image, v_uv);
    } else if (kind < 1.5) {
        float d = texture(image, v_uv / vec2(textureSize(image, 0))).r;
        float w = max(fwidth(d), 0.0001) * 0.7;
        c.a *= smoothstep(0.5 - w, 0.5 + w, d);
    } else if (kind > 3.5) {
        // solid: the vertex colour; polygons fade their edges through transparent outer vertices
    } else {
        float r = min(v_shape.x, min(v_local_half.z, v_local_half.w));
        float d = box(v_local_half.xy, v_local_half.zw, r);
        float a;
        if (v_shape.z > 0.0) {
            a = 1.0 - smoothstep(-v_shape.z, v_shape.z, d);
        } else if (v_shape.y > 0.0) {
            a = clamp(0.5 - (abs(d + v_shape.y * 0.5) - v_shape.y * 0.5), 0.0, 1.0);
        } else {
            a = clamp(0.5 - d, 0.0, 1.0);
        }
        if (kind > 2.5) {
            c *= texture(image, v_uv);
        }
        c.a *= a;
    }
    result = vec4(c.rgb * c.a, c.a);
}
