# Khronos registries

The machine-readable API registries `std::graphics::opengl` and `std::graphics::vulkan` are made
from (see `compiler/src/graphics_registry.cpp` and `docs/design/stdlib/graphics/graphics.md`).
edaxc embeds them when it is built; `edaxc registry opengl|vulkan` prints the modules made from them.

| File | Source | Version | License |
|---|---|---|---|
| `gl.xml` | [KhronosGroup/OpenGL-Registry](https://github.com/KhronosGroup/OpenGL-Registry), `xml/gl.xml` at 6af574a (2026-10-06) | OpenGL 4.6, OpenGL ES 3.2 | Apache-2.0 |
| `vk.xml` | [KhronosGroup/Vulkan-Docs](https://github.com/KhronosGroup/Vulkan-Docs), `xml/vk.xml` at f0fe740 (2026-11-22) | Vulkan 1.4.369 | Apache-2.0 OR MIT |

Both files are Copyright The Khronos Group Inc. and are used unchanged. To update an API, replace
the file with a newer one from the same repository and rebuild edaxc.
