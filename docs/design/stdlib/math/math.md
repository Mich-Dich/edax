# Math: `std::math`

`std::math` has the vectors, rectangles and colours that a UI and a 2-D renderer need. It is the first part of the math library of the engine plan; matrices, quaternions and bounds follow. Every type is a copy type of `f32`s with C layout, so arrays of them go to the GPU as they are. The module is loaded when a program names `std::math` (or `std::ui`, which uses it).

## Rules

### Vectors
| Type | Fields | Methods |
|---|---|---|
| `vec2` | `x`, `y` | `splat`, `dot`, `length`, `length_squared`, `normalized`, `perpendicular`, `abs`, `min`, `max`, `lerp`; `+ - *` (by a vector or an `f32`), `/` (by an `f32`), unary `-`, `==` |
| `vec3` | `x`, `y`, `z` | `splat`, `dot`, `cross`, `length`, `normalized`, `lerp`; `+ - * /`, unary `-`, `==` |
| `vec4` | `x`, `y`, `z`, `w` | `splat`, `dot`, `lerp`; `+ - *`, `==` |

- `normalized()` of the zero vector is the zero vector, not NaNs.
- Free functions: `lerp(a, b, t)`, `smoothstep(edge0, edge1, x)`, and the constant `pi`.

### Rectangles
`rect` is a top-left corner and a size (`x`, `y`, `width`, `height`), y down as on screens.
- `rect::from_corners(a, b)` (any order), `rect::at(origin, size)`.
- `left`, `top`, `right`, `bottom`, `origin`, `size`, `center`.
- `contains(p)`: the left and top edges are inside, the right and bottom edges aren't, so rectangles that share an edge don't both contain a point on it.
- `overlaps`, `intersection` (size 0 when they don't overlap), `united`, `shrunk(d)` (never below size 0), `expanded(d)`, `translated(v)`, `is_empty`.

### Colours
`color` is sRGB with straight alpha, every channel in [0, 1]: what colour pickers, CSS and image files mean by a colour.

| Make | Use |
|---|---|
| `color::hex(0x4F7CFF)`, `hex_alpha(0x4F7CFF80)`, `rgb8`, `rgba8` | `with_alpha(a)`, `packed()` (bytes r, g, b, a with r lowest: `GL_RGBA` + `GL_UNSIGNED_BYTE`) |
| `color::from_oklch(c, alpha)`, `white()`, `black()`, `transparent()` | `to_oklch()`, `mix(to, t)`, `lighter(amount)` |
| | `luminance()` (WCAG 2), `contrast(other)` (1 to 21) |

- **`oklch`** (`l`, `c`, `h` in degrees) is the cylindrical form of OKLab: equal steps of `l` look equal, which makes it the space themes derive their shades in.
- `from_oklch` keeps lightness and hue of a colour outside sRGB and lowers its chroma until it fits (a binary search), instead of clipping channels, which would change the hue.
- `mix` works in OKLab: the middle of two colours of equal lightness keeps that lightness. Alpha mixes linearly.
- `srgb_to_linear` and `linear_to_srgb` convert one channel.

## Example
```edax
namespace m = std::math;

m::color accent = m::color::hex(0x4F7CFF);
m::oklch c = accent.to_oklch();                                  // l 0.625, c 0.201, h 266
m::color hover = m::color::from_oklch(m::oklch{ .l = c.l + 0.05, .c = c.c, .h = c.h }, 1.0);
bool readable = m::color::white().contrast(accent) >= 3.0;
m::rect r = m::rect{ .x = 10.0, .y = 10.0, .width = 100.0, .height = 40.0 };
m::rect inner = r.shrunk(4.0);
```

## Rationale
- **sRGB with straight alpha as the colour type:** it is what people write and what files contain. Linear light and premultiplied alpha are the renderer's business.
- **OKLCH for derived colours:** HSL's lightness isn't perceptual (yellow and blue at "50 %" look nothing alike), so themes made in it need hand-tuning per hue.
- **A small first part:** `std::math` grows with the engine work (matrices, quaternions, transforms, bounds); the UI needed only these.
