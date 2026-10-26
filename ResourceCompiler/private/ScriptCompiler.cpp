#include "ScriptCompiler.hpp"

#include <lua.hpp>

#include "Core/Base.hpp"
#include "Memory/Stack.hpp"

namespace ox::rc {
static auto append_bytecode(lua_State*, const void* data, usize size, void* user_data) -> i32 {
  auto* bytecode = static_cast<std::vector<u8>*>(user_data);
  const auto* bytes = static_cast<const u8*>(data);
  bytecode->insert(bytecode->end(), bytes, bytes + size);

  return 0;
}

auto script_bytecode_version() -> u64 { return LUA_VERSION_RELEASE_NUM; }

auto compile_script(std::span<const u8> source, std::string_view chunk_name)
  -> std::expected<std::vector<u8>, std::string> {
  ZoneScoped;
  memory::ScopedStack stack;

  // nothing runs here, so a bare state with no libraries opened is enough to parse
  auto* state = luaL_newstate();
  if (!state) {
    return std::unexpected("Couldn't create a lua state.");
  }
  OX_DEFER(&) { lua_close(state); };

  // text only: a precompiled chunk passed off as a source would go straight into the pack unchecked
  const auto* source_chars = reinterpret_cast<const c8*>(source.data());
  const auto* name = stack.null_terminate_cstr(chunk_name);
  if (luaL_loadbufferx(state, source_chars, source.size(), name, "t") != LUA_OK) {
    return std::unexpected(std::string(lua_tostring(state, -1)));
  }

  auto bytecode = std::vector<u8>{};
  if (lua_dump(state, append_bytecode, &bytecode, 0) != 0 || bytecode.empty()) {
    return std::unexpected("Couldn't dump the compiled chunk.");
  }

  return bytecode;
}
} // namespace ox::rc
