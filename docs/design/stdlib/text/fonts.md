# Fonts: `std::text::font`

`std::text::font` reads TrueType fonts and renders glyphs as signed distance fields, written in Edax. It is what `std::ui` draws text with, and the base for in-world text and debug drawing. The module is loaded when a program names `std::text::font` (or `std::ui`).

## Rules

### Fonts
- `std::text::font::parse(data)` reads a TrueType font (`glyf` outlines; CFF-based OpenType fonts are `font_error::not_truetype`). The font keeps a copy of the file. Errors: `not_truetype`, `missing_table` (head, hhea, hmtx, maxp, loca, glyf or cmap), `no_unicode_map` (needs a format 4 or 12 Unicode map), `damaged` (a table outside the file).
- **Built-in fonts:** `font::default_sans()` and `default_sans_semibold()` (DM Sans 400 and 600, SIL Open Font License) and `default_mono()` (DejaVu Sans Mono, Bitstream Vera licence). They are cut to Latin-1, Latin Extended-A and a few symbols (`– — ‘ ’ “ ” • … € ™ ← ↑ → ↓ −`), with kerning in a `kern` table. They are built into edaxc: `library/std/text/fonts` has the files, their licences and `make_fonts.py`, which makes them from the original fonts.

| Method | Result |
|---|---|
| `glyph_index(c)` | The glyph of a `char`, 0 (the "missing" glyph) when the font has none |
| `glyph_count()` | |
| `metrics()` | `line_metrics`: ascent (positive), descent (negative), line gap, in ems |
| `advance(glyph)` | How far the pen moves, in ems |
| `kerning(left, right)` | Extra advance between two glyphs, in ems (negative: closer). From the `kern` table only |
| `outline_of(glyph, pixel_size)` | The outline as closed polygons in ems: curves split into lines fine enough for that pixel size. Composite glyphs (é = e + ´) are assembled with their transforms |
| `render_distance_field(glyph, pixel_size, spread)` | A `distance_field` (below) |

### Distance fields
- A `distance_field` is a small 8-bit image: 128 on the outline, 255 `spread` pixels inside, 0 `spread` pixels outside. `left` and `top` place it relative to the pen on the baseline (y down).
- Inside and outside use the non-zero winding rule, like TrueType rasterizers.
- A GPU draws a glyph from its distance field at any size with one texture sample and a `smoothstep`, which is how one atlas at 40 pixels serves every text size of `std::ui`.

### Not yet
Text shaping (ligatures, Arabic, Indic scripts), hinting, colour fonts and multi-channel distance fields (sharp corners at very large sizes) are later steps; the UI roadmap lists them.

## Example
```edax
std::text::font sans = std::text::font::default_sans();
u16 g = sans.glyph_index('A');
f32 width = sans.advance(g) * 14.0;                               // pixels at 14 px
std::text::distance_field field = sans.render_distance_field(g, 40.0, 5.0);
```

## Rationale
- **Distance fields over coverage bitmaps:** a UI with a type scale, HiDPI screens and a zoomable node editor needs text at many sizes; distance fields need one rendering per glyph instead of one per size.
- **Fonts in the library:** a program can show text with no font files of its own, and every program looks the same on every machine.
- **Only `kern` kerning:** reading GPOS means a shaping engine; the bundled fonts' GPOS pairs are converted to a `kern` table when they are made.
