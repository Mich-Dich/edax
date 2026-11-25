#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 local_half;
layout(location = 4) in vec4 shape;
uniform vec2 screen;
out vec2 v_uv;
out vec4 v_color;
out vec4 v_local_half;
out vec4 v_shape;
void main() {
    v_uv = uv;
    v_color = color;
    v_local_half = local_half;
    v_shape = shape;
    gl_Position = vec4(position.x / screen.x * 2.0 - 1.0, 1.0 - position.y / screen.y * 2.0, 0.0, 1.0);
}
