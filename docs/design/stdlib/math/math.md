# Math: `std::math`

`std::math` is the math of games and renderers: vectors, matrices, quaternions, transforms, projections, shapes for culling and picking, curves, easing and noise, plus the rectangles and colours a UI needs. It reads like GLM and GLSL (`dot(a, b)`, `normalize(v)`, `inverse(m)`, column-major matrices), so shader code and CPU code look the same, and it is at least as fast as GLM compiled with GCC or Clang (see [Performance](#performance)).

```edax
namespace m = std::math;

m::mat4 view       = m::look_at(eye, target, m::vec3::unit_y());
m::mat4 projection = m::perspective(m::deg(60.0), aspect, 0.1, 500.0);
m::mat4 model      = m::mat4::trs(position, m::quat::from_axis_angle(m::vec3::unit_y(), m::deg(45.0)), m::vec3::one());
m::mat4 mvp        = projection * view * model;
```

## Rules

### 1. Number types
Every type takes its number type as a template argument, and **`f32` when none is written**: `vec3` is `vec3<f32>`, `mat4` is `mat4<f32>`; `vec3<f64>`, `vec2<i32>`, `mat4<f64>` are the others. Vectors work on every number type; matrices, quaternions, transforms and shapes on `f32` and `f64`.
- Functions follow their arguments: `m::length(v)` returns `f64` for a `vec3<f64>`. A call with only literals is `f32`: `m::lerp(0.0, 10.0, 0.25)`. For integers write one suffix: `m::max(1i32, 2)`.
- `v.to<f64>()`, `m.to<f64>()`, `q.to<f64>()` convert between number types; a float becomes an integer by dropping its fraction, clamped to the integer's range.
- There is no implicit conversion between `vec3` and `vec3<f64>`.
- All types are plain copy types with C layout and no padding (`vec3` is 12 bytes, `mat4` is 64), so arrays of them go to the GPU as they are; `as_span()` gives the numbers for C and graphics APIs.
- Everything that doesn't touch memory works at compile time: `compile_time m::mat4 fixed = m::mat4::translation(...) * m::mat4::scaling(...);`.

### 2. Angles
Angles have their own types, so degrees are never passed for radians:

| | |
|---|---|
| `m::radians` | What every trigonometric and rotation function takes. A distinct `f32` |
| `m::degrees` | For values people edit; `m::to_radians(d)`, `m::to_degrees(r)` convert |
| `m::deg(90.0)` | 90 degrees **as radians**: the usual way to write an angle |
| `m::rad(1.57)` | A number of radians as an angle |

- `m::sin(45.0)` is a compile error (a number is not an angle); `m::sin(m::deg(45.0))` is 0.7071.
- Angles add, subtract, negate and compare; `a * 2.0` scales, `a / b` is an `f32`, `cast<f32>(a)` is the number of radians.
- `sin`, `cos`, `tan` take an angle; `asin`, `acos`, `atan`, `atan2(y, x)` return one.
- `wrap_angle(a)` (into [-π, π)), `angle_difference(from, to)` (the short way), `lerp_angle(a, b, t)`.
- Angles are `f32`, also with `f64` matrices: the sine and cosine are computed in `f64`, of an angle with `f32` precision.
- Constants: `pi`, `tau`, `half_pi`, `e`, `sqrt_2`, `epsilon` (`f32`); `pi_f64`, `tau_f64`.

### 3. Numbers
`abs sign min max clamp saturate floor ceil round trunc fract mod sqrt inverse_sqrt pow exp exp2 log log2 lerp inverse_lerp remap step smoothstep smootherstep approx_equal wrap ping_pong`, on scalars and (component by component) on vectors.
- `lerp(a, b, t)` is GLSL's `mix`. `mod(x, y)` has the sign of `y`, as in GLSL.
- `min`, `max` and `clamp` compile to one instruction each (no NaN handling, no check that `lo <= hi`).
- `damp(current, target, rate, dt)` is the frame-rate independent "lerp towards": `position = m::damp(position, goal, 8.0, dt)`. `move_towards(current, target, max_step)` never overshoots. `smooth_damp(current, target, mut velocity, smooth_time, dt)` is a critically damped spring (camera follow).

### 4. Vectors
`vec2`, `vec3`, `vec4` with the fields `x`, `y`, `z`, `w`.

| | |
|---|---|
| Make | `vec3::make(1.0, 2.0, 3.0)`, `vec3{ .x = 1.0 }`, `splat(v)`, `zero()`, `one()`, `unit_x()` ..., `vec3::from_xy(v2, z)`, `vec4::from_xyz(v3, w)` |
| Parts | `v.xy()`, `v.xz()`, `v.yz()` (vec3); `v.xyz()`, `v.xy()`, `v.zw()` (vec4); `v[i]`; `v.as_span()` |
| Operators | `+ - * /` with a vector (component by component) or a number, `k * v`, unary `-`, `==`, and `+= -= *= /=` |
| Functions | `dot cross length length_squared distance distance_squared normalize normalize_or_zero reflect refract face_forward project reject angle perpendicular rotate min_component max_component` |
| Comparisons | `less_than(a, b)` ... `equal`, `not_equal` give a `vec3<bool>`; `any`, `all`, `select(pick, a, b)` |

- `normalize` doesn't look at the length (a zero vector gives NaNs); `normalize_or_zero` returns the zero vector for it.
- `cross` of two `vec2`s is the z of the 3D cross product: positive when `b` is counter-clockwise from `a`.
- Printing: `(1.0, 2.0, 3.0)`.

### 5. Matrices
`mat2`, `mat3`, `mat4`: **column-major**, as in GLSL and GLM. `m.c0` ... `m.c3` are the columns, `m[1]` is the second column, `m * v` transforms a column vector, `a * b` applies `b` first. A matrix written without values is the identity.

| | |
|---|---|
| Make | `identity()`, `zero()`, `from_columns(...)`, `from_rows(...)`, `from_diagonal(v)` |
| `mat4` | `translation(v)`, `rotation(angle, axis)`, `rotation_x/y/z(angle)`, `scaling(v)`, `from_quat(q)`, `from_mat3(m)`, `trs(translation, rotation, scale)` |
| `mat3` | `rotation(angle, axis)`, `rotation_x/y/z`, `scaling`, `from_quat`, `from_mat4`; for 2D: `translation_2d(v)`, `rotation_2d(angle)`, `scaling_2d(v)` |
| `mat2` | `rotation(angle)`, `scaling(v)` |
| Operators | `m * m`, `m * v`, `m + m`, `m - m`, `m * k`, `k * m`, `m / k`, unary `-`, `==` |
| Functions | `transpose determinant inverse affine_inverse normal_matrix approx_equal` |
| Combining | `translate(m, v)`, `rotate(m, angle, axis)`, `scale(m, v)`: `m` followed by the operation in `m`'s own space, as in GLM |
| Applying | `transform_point(m, p)` (w = 1), `transform_vector(m, v)` (w = 0), `project_point(m, p)` (divides by w); with a `mat3`, the same for 2D points |

- `affine_inverse` is for matrices whose last row is (0, 0, 0, 1), which is every model and view matrix: about half the work of `inverse`.
- `inverse` doesn't check the determinant: a matrix without an inverse gives infinities.
- Rotations are counter-clockwise when the axis points at the viewer (right-handed). Axes must be unit vectors.

### 6. Projections and views
The plain names are **right-handed with clip-space depth from 0 to 1**: Vulkan, Direct3D, Metal, WebGPU, and OpenGL with `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`. The camera looks along -z, y is up.

| Function | |
|---|---|
| `perspective(fov_y, aspect, near, far)` | The same as `perspective_rh_zo` |
| `perspective_rh_no`, `perspective_lh_zo`, `perspective_lh_no` | `_lh`: left-handed (the camera looks along +z). `_no`: depth from -1 to 1 (OpenGL's default) |
| `perspective_infinite(fov_y, aspect, near)` | No far plane |
| `perspective_reversed(...)`, `perspective_infinite_reversed(fov_y, aspect, near)` | Near is depth 1, far is 0: with a float depth buffer and a `greater` test, precision is even over the whole distance. The usual choice for large worlds |
| `perspective_off_center(left, right, bottom, top, near, far)` | From the edges of the near plane (GLM's `frustum`): VR eyes, tiles, jitter |
| `ortho(left, right, bottom, top, near, far)`, `ortho_rh_no`, `ortho_lh_zo`, `ortho_lh_no`, `ortho_2d(left, right, bottom, top)` | |
| `look_at(eye, target, up)`, `look_at_lh` | The view matrix of a camera |

- Clip-space y points up. Vulkan's points down: use a negative viewport height, or negate `projection.c1.y`.

### 7. Quaternions and transforms
`quat` is a rotation as a unit quaternion (`x`, `y`, `z`, `w`; `w` is the real part; no values is no rotation).

| | |
|---|---|
| Make | `identity()`, `from_axis_angle(axis, angle)`, `rotation_x/y/z(angle)`, `from_euler(pitch, yaw, roll)`, `from_mat3(m)`, `from_mat4(m)`, `look_rotation(forward, up)`, `rotation_between(from, to)` |
| Use | `q * q` (the right one first), `q * v` (rotates a `vec3`), `conjugate`, `inverse`, `normalize`, `dot`, `length`, `nlerp`, `slerp`, `angle(a, b)` |
| Read | `q.axis()`, `q.angle()`, `q.to_euler()`, `q.forward()`, `q.right()`, `q.up()` |

- Euler angles are pitch (around x), yaw (around y), roll (around z), applied roll first, then pitch, then yaw: a first-person camera with y up. `to_euler` returns the `euler_angles` that `from_euler` takes.
- `nlerp` is several times cheaper than `slerp`, follows the same path and is only slightly uneven in speed: the usual choice for blending animations.

`transform` is a translation, a rotation and a scale: what a scene keeps per object (40 bytes instead of 64, it interpolates, and it never drifts into a shear).
- `parent * child` puts the child into the parent's space; `to_mat4()` is the matrix for the GPU; `transform::from_mat4(m)` takes a matrix apart.
- `transform_point(t, p)`, `transform_vector(t, v)`, `inverse(t)`, `lerp(a, b, t)`, `forward()`, `right()`, `up()`.
- `*` and `inverse` are exact for uniform scales (a non-uniform scale on a rotated child is a shear, which a transform can't hold).

### 8. Shapes
`aabb` (`min`, `max`), `sphere` (`center`, `radius`), `plane` (`normal`, `d`: the points with `dot(normal, p) + d == 0`), `ray` (`origin`, `direction`), `frustum` (six planes, normals inwards).

| | |
|---|---|
| Make | `aabb::from_center_extents(c, e)`, `aabb::from_points(span)`, `aabb::empty()`, `sphere::make(c, r)`, `plane::from_point_normal(p, n)`, `plane::from_points(a, b, c)`, `ray::make(o, d)`, `frustum::from_view_projection(m)` (and `_no` for depth -1 to 1) |
| `intersects(a, b)` | box/box, sphere/sphere, box/sphere, frustum/box, frustum/sphere |
| `contains(shape, x)` | a point in a box, sphere or frustum; a box in a box |
| `intersect(ray, shape)` | `std::option<T>`: the `t` of the first hit on a plane, sphere or box; `intersect_triangle(ray, a, b, c)` |
| `closest_point(shape, p)` | on a box, sphere, plane or ray |
| Others | `signed_distance(plane, p)`, `normalize(plane)`, `merge(box, box)`, `merge(box, point)`, `transform_aabb(m, box)`, `bounding_sphere(box)`, `box.center()`, `extents()`, `size()`, `ray.at(t)` |
| `cull(frustum, shapes, visible)` | Frustum culling of a span of boxes or spheres into a span of `bool`s; returns how many are visible |

- A frustum made from `projection * view` is in world space; from `projection * view * model`, in the object's space.
- Frustum tests are conservative, as culling wants: a shape just outside a corner can count as visible.

### 9. Curves
`bezier(p0, p1, p2, t)` (quadratic), `bezier(p0, p1, p2, p3, t)` (cubic), `bezier_tangent`, `hermite(p0, m0, p1, m1, t)`, `catmull_rom(p0, p1, p2, p3, t)`: on numbers and vectors.

### 10. Easing: `std::math::ease`
`in_`, `out_` and `in_out_` forms of `quad cubic quart quint sine expo circ back elastic bounce`: `t` from 0 to 1 in, the eased progress out. `m::lerp(from, to, ease::out_cubic(t))`.

### 11. Noise: `std::math::noise`
| Function | |
|---|---|
| `value(p)`, `perlin(p)`, `simplex(p)` | About -1 to 1, for a `vec2` or `vec3`; one feature per unit |
| `worley(p)` | The distance to the nearest feature point (cells, cracks) |
| `fbm(p, octaves)`, `fbm(p, octaves, lacunarity, gain, seed)` | Layered Perlin noise: terrain, clouds |
| `hash(x)`, `hash(x, y, seed)`, `hash(x, y, z, seed)`, `unit(h)` | A well-mixed `u32`, and a hash as a number in [0, 1) |

- Every function takes an optional `seed` and is a pure function of its input: the same value on every machine and thread, in any order, so a world can be generated in pieces.
- The lattice is hashed, not looked up in a permutation table: the noise doesn't repeat every 256 units.

### 12. Rectangles and colours
`rect` is a top-left corner and a size (`x`, `y`, `width`, `height`), y down as on screens.
- `rect::from_corners(a, b)` (any order), `rect::at(origin, size)`; `left`, `top`, `right`, `bottom`, `origin`, `size`, `center`.
- `contains(p)`: the left and top edges are inside, the right and bottom edges aren't, so rectangles that share an edge don't both contain a point on it.
- `overlaps`, `intersection` (size 0 when they don't overlap), `united`, `shrunk(d)` (never below size 0), `expanded(d)`, `translated(v)`, `is_empty`.

`color` is sRGB with straight alpha, every channel in [0, 1]: what colour pickers, CSS and image files mean by a colour.

| Make | Use |
|---|---|
| `color::hex(0x4F7CFF)`, `hex_alpha(0x4F7CFF80)`, `rgb8`, `rgba8` | `with_alpha(a)`, `packed()` (bytes r, g, b, a with r lowest: `GL_RGBA` + `GL_UNSIGNED_BYTE`) |
| `color::from_oklch(c, alpha)`, `white()`, `black()`, `transparent()` | `to_oklch()`, `mix(to, t)`, `lighter(amount)` |
| `color::from_vec4(v)`, `color::from_linear(v)` | `to_vec4()`, `to_linear()` (linear light, for shaders), `luminance()` (WCAG 2), `contrast(other)` (1 to 21) |

- **`oklch`** (`l`, `c`, `h` in degrees) is the cylindrical form of OKLab: equal steps of `l` look equal, which makes it the space themes derive their shades in.
- `from_oklch` keeps lightness and hue of a colour outside sRGB and lowers its chroma until it fits, instead of clipping channels, which would change the hue.
- `mix` works in OKLab; alpha mixes linearly. `srgb_to_linear` and `linear_to_srgb` convert one channel.

## Examples

### 1. A camera and a draw call
```edax
namespace m = std::math;

m::quat   facing = m::quat::from_euler(m::deg(pitch), m::deg(yaw), m::rad(0.0));
m::vec3   eye    = player.position + m::vec3::make(0.0, 1.7, 0.0);
m::mat4   view   = m::look_at(eye, eye + facing.forward(), m::vec3::unit_y());
m::mat4   proj   = m::perspective_infinite_reversed(m::deg(70.0), width / height, 0.1);
m::mat4   vp     = proj * view;
glUniformMatrix4fv(at, 1, GL_FALSE, vp.as_span().as_ptr());
```

### 2. A scene: parents, bounds, culling
```edax
world[0] = nodes[0].local.to_mat4();
for (usize i : 1..nodes.length()) {
    world[i]  = world[nodes[i].parent] * nodes[i].local.to_mat4();   // parents come first
    bounds[i] = m::transform_aabb(world[i], nodes[i].box);
}
m::frustum camera = m::frustum::from_view_projection(vp);
usize seen = m::cull(camera, bounds.as_span(), visible.as_mut_span());
```

### 3. Picking with a ray
```edax
m::ray pick = m::ray::make(eye, direction);
match (m::intersect(pick, m::plane::from_point_normal(m::vec3::zero(), m::vec3::unit_y()))) {
    std::some(t) => place_marker(pick.at(t)),
    std::none => {},
}
```

### 4. Smooth motion and terrain
```edax
namespace ease = std::math::ease;
namespace noise = std::math::noise;

camera_position = m::damp(camera_position, target, 8.0, dt);             // the same at 30 and 144 fps
panel_x         = m::lerp(-300.0, 0.0, ease::out_back(progress));
f32 height      = noise::fbm(m::vec2::make(x, z) * 0.01, 5) * 40.0;
```

### 5. Colours
```edax
m::color accent = m::color::hex(0x4F7CFF);
m::oklch c = accent.to_oklch();                                  // l 0.625, c 0.201, h 266
m::color hover = m::color::from_oklch(m::oklch{ .l = c.l + 0.05, .c = c.c, .h = c.h }, 1.0);
bool readable = m::color::white().contrast(accent) >= 3.0;
```

## Performance
Measured against GLM 1.0.1 (GCC 13 and Clang 18 at `-O2`) on the x86-64 baseline (SSE2), 1024 elements per kernel so the data stays in the cache. Time relative to the faster of the two C++ builds; below 1 is faster than GLM.

| Kernel | Edax / GLM | | Kernel | Edax / GLM |
|---|---|---|---|---|
| `mat4 * mat4` | 0.9 - 1.1 | | `quat * quat` | 1.0 |
| `mat4 * vec4` | 1.0 | | `quat * vec3` | 0.65 |
| `transform_point` | 0.85 | | `normalize(vec3)` | 0.65 |
| `inverse(mat4)` | 0.55 | | `nlerp` | 1.0 |
| `affine_inverse` | 0.8 | | `slerp` | 1.1 (the C library's sine and arc cosine) |
| `mat4::trs` | 0.4 | | a tree of 20k matrices, culled (`bench/edax/math_scene.ed`) | 0.55 |

- **Debug builds stay usable:** the same kernels are 3 to 9 times slower in `debug` than in `release`, and within 20 % in `development`. GLM at `-O0` is 60 to 100 times slower than at `-O2` on the matrix kernels.
- **Noise** is 5 to 10 times faster than GLM's and within 0 to 35 % of FastNoiseLite's (4 million samples: Perlin 2D 47 ms, 3D 75 ms; simplex 2D 67 ms, 3D 114 ms; five octaves of fbm 226 ms).
- **Culling** 82 million boxes takes 0.5 s (6 ns per box), 1.5 to 2 times faster than the plain C++ loop.
- What makes it so: the compiler's SLP vectorizer turns the four multiplies of a vector operation into one (it is on in every optimized build); `mat4 * vec4` and `mat4 * mat4` use `std::simd` directly; the products that matter are always inlined; `cull` tests one shape against all six planes with two SIMD registers; the noise functions look their gradients up in tables instead of branching on hash bits and convert with `cast::truncate`.

## Rationale
- **Free functions, as in GLSL and GLM:** `dot(a, b)` reads like the formula, works the same on every vector size, and code moves between shaders and the CPU unchanged. Members are for making values and for a type's own parts (`v.xy()`, `m.row(i)`, `q.forward()`).
- **Typed angles:** mixing up degrees and radians is the classic bug of every math library that takes a plain float; a distinct type costs nothing at run time and `m::deg(45.0)` is as short as it gets.
- **Right-handed, depth 0 to 1 as the default:** it is what every current graphics API except classic OpenGL uses, and the other conventions are a suffix away instead of a compile-time switch that silently changes every matrix in the program (GLM's `GLM_FORCE_DEPTH_ZERO_TO_ONE`).
- **Generic with an `f32` default:** games use `f32` almost everywhere, so the short names mean that; `f64` positions for large worlds and integer vectors for grids are the same types, not a second library.
- **`normalize` without a check:** the common case shouldn't pay for a branch; `normalize_or_zero` is one word away where the input can be zero.
- **`transform` next to `mat4`:** hierarchies, animation and networking want the three parts; the GPU wants the matrix. `to_mat4` builds it directly, cheaper than three matrix products.
- **Hashed noise:** a permutation table repeats, needs a seeded global, and its lookups aren't faster than three multiplies. A pure function of (position, seed) can run on any thread.
- **sRGB with straight alpha as the colour type:** it is what people write and what files contain. `to_linear` is there for the renderer. OKLCH for derived colours, because HSL's lightness isn't perceptual.
- **Not included:** `mat3x4` and other non-square matrices, the full set of swizzles (`v.zyx()`), packed GPU formats (half floats, 10-10-10-2 normals), and `f64` angles.
