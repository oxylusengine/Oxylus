#include "Render/Slang/Slang.hpp"

#include "Core/App.hpp"

namespace ox {
void Slang::create_session(this Slang& self, const SessionInfo& session_info) {
  ZoneScoped;
  auto& ctx = App::get_vkcontext();

  self.slang_session = ctx.shader_compiler.new_session(
    {.optimizaton_level = std::to_underlying(session_info.optimization_level),
     .definitions = session_info.definitions,
     .root_directory = session_info.root_directory}
  );
}

} // namespace ox
