#pragma once
// The graphics API mapper: turns a Khronos API registry (gl.xml, vk.xml) into a module of the
// standard library, so OpenGL and Vulkan are described by the registry instead of by hand.
//
//   std::graphics::opengl   from gl.xml: every GL_* constant of OpenGL 1.0-4.6 and OpenGL ES
//                           2.0-3.2, and the class template functions<std::graphics::api A>
//   std::graphics::vulkan   from vk.xml: the constants, types and structs of Vulkan 1.0-1.4 and
//                           the window-system extensions, and functions<std::graphics::api A>
//
// functions<A> has a method for every command of the registry's API family. A method that the
// program's API version (A) doesn't have fails a compile_time check when it is called, naming the
// versions that have it; load() looks up exactly the commands A has. The registries are
// embedded in edaxc (library/std/graphics/registry), so a new API version is a new registry file.

#include <string>
#include <string_view>

namespace edax {

// A module of the standard library made from a registry: a header and a source.
struct GeneratedStdModule {
    std::string header;
    std::string source;
    // Commands, constants and types written, and the registry's own version ("1.4.369").
    std::size_t commands  = 0;
    std::size_t constants = 0;
    std::size_t types     = 0;
    std::string registry_version;
};

// std::graphics::opengl from the text of gl.xml.
GeneratedStdModule generate_opengl_module(std::string_view gl_xml);

// std::graphics::vulkan from the text of vk.xml.
GeneratedStdModule generate_vulkan_module(std::string_view vk_xml);

// The registry embedded in edaxc: "gl.xml" or "vk.xml".
std::string_view std_registry(std::string_view file);

} // namespace edax
