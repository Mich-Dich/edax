# Converting the package 'plat'

`edaxc package convert plat` read the package's CMake files for linux, windows and macos and wrote `packages/plat.ed`. This report says what each command became.

## Options

None (each option at its default).

The files were read with `CMAKE_BUILD_TYPE=Release`; feature checks (`check_include_file`, ...) were answered by this machine's compiler for every platform.

## Files read

- `CMakeLists.txt`

## Commands

| Where | Command | Became |
|---|---|---|
| `CMakeLists.txt:4` | `add_library` | the target plat |

## TODOs

- `CMakeLists.txt:3` the command 'execute_process' isn't understood by the CMake reader. The reader doesn't translate it: write what it does with build-API calls (a `g.add_step` for a command that makes files), or leave it out if the build doesn't need it.
