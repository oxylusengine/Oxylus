# Local changes to imgui-node-editor

Vendored from https://github.com/thedmd/imgui-node-editor at `021aa0ea`.

- `crude_json.cpp`: added `#include <exception>`. libc++ does not pull `std::terminate` in
  transitively, so the file does not compile without it.
