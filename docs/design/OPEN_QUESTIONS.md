# Open Questions

What isn't decided yet. Every other topic is described in the pages.

## Build output ([Build output and diagnostics](build/build-output.md))
1. **Waiting rows in the live area.** When a target's table doesn't fit on the screen, only its running rows are shown. Alternative: a window of rows around the running ones, so the table visibly fills up.
2. **Order of finished tables.** A table is printed when its target finishes, so with targets built in parallel the scrollback is in finishing order. Alternative: print them in `build.ed`'s order at the end.
3. **The timings file:** the last build's unit times only, or a history for better weights and an `{eta}` placeholder?
4. **Patterns as `std::log::pattern`** (checked when `build.ed` is compiled, with colors) instead of plain strings; a user's own defaults in `~/.config/edax/output.ed`.
5. **Finer units for Edax targets:** the files are done together after `check`; checking module by module would let each source row finish on its own.

## Language
6. **A garbage collector, far in the future.** Edax is built on ownership and RAII, and a collector is not planned. If one ever comes, it would be opt-in and visible in the type (`std::gc<T>` from a collected heap), never a global switch, so code that doesn't use it pays nothing.
7. **Packed structs and bit fields:** `@std::layout{ .packed = true }` is designed ([Memory layout](language/user-types/layout.md)); whether bit fields are a language feature (`u32 kind : 4;`) or a library type built with field-generating templates is open. The library version is preferred.
8. **`defer`:** a scope-exit block for C interop code without RAII wrappers. Added only if the C interop examples show the need; RAII stays the one way for Edax types.
9. **Field-generating templates and name suffixes:** whether `f.name ~ "_dirty"` (a generated name with a written suffix) is allowed, or only `f.name` itself.
10. **Resumable functions and threads:** whether a `resumable<T>` may move between threads when its captures are sendable (like a lambda), and what `std::waker` is on each platform.

