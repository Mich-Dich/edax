# Images: `std::image`

`std::image` keeps images in memory and reads and writes PNG and QOI files, written in Edax with no outside library. The module is loaded when a program names `std::image` (or `std::ui`).

## Rules

### `rgba8`
- `std::image::rgba8` is `width`, `height` and `pixels`: `width * height * 4` bytes, 8-bit RGBA with straight alpha, rows top to bottom. Plain data: an image can be decoded on a worker thread and sent to the thread that uploads it.
- `rgba8::filled(w, h, r, g, b, a)`, `pixel(x, y)` (packed, r lowest), `set_pixel(x, y, packed)`.

### Decoding
| Function | Reads |
|---|---|
| `decode_png(file)` | PNG: grey, grey + alpha, RGB, RGBA, palette (1, 2, 4, 8 bits) with `tRNS` transparency, 8 and 16 bits per sample (16 bits keep the high byte). Not interlaced |
| `decode_qoi(file)` | QOI |

- Both return `std::result<rgba8, decode_error>`: `wrong_format` (not a file of that format), `damaged`, `unsupported` (interlaced PNG), `too_large` (above 32768 pixels a side).
- `inflate_zlib(data)` is the zlib/deflate decoder PNG uses (stored, fixed and dynamic Huffman blocks), for other formats that need it. Checksums are not verified.

### Encoding
- `encode_png(image)`: RGBA, 8 bits, with **stored** deflate blocks: correct PNGs that every program reads, but not compressed. Meant for screenshots and tests.
- `encode_qoi(image)`: QOI, which compresses well and fast.

### At compile time
`std::image::decode_png(std::embed("logo.png"))` works in run-time code today. Decoding at compile time (pixels baked into the binary) waits for the faster compile-time evaluator (UI roadmap M8).

## Example
```edax
std::image::rgba8 logo = std::image::decode_png(std::embed("assets/logo.png"))?;
ui::texture t = renderer.upload(logo);

std::image::rgba8 shot = renderer.read_pixels(frame);
std::darray<u8> png = std::image::encode_png(shot);
```

## Rationale
- **One pixel format:** every decoder ends in 8-bit RGBA, so code that uses images (uploads, UI) has one case. Formats with more precision (HDR, 16-bit) will get their own types.
- **PNG and QOI in std:** a UI library needs icons and pictures without a package; JPEG, KTX2 and other formats come as packages.
- **Stored blocks when writing PNG:** a compressing encoder is larger than the rest of the module; screenshots and test images don't need it.
