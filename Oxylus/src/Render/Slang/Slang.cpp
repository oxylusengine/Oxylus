#include "Slang.hpp"

#include <vuk/runtime/vk/Pipeline.hpp>

#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "Render/Vulkan/VkContext.hpp"

namespace ox {
void Slang::create_session(this Slang& self, const SessionInfo& session_info) {
  ZoneScoped;
  auto& ctx = App::get_vkcontext();

  self.slang_session = ctx.shader_compiler.new_session(
      {.definitions = session_info.definitions, .root_directory = session_info.root_directory});
}

void Slang::add_shader(this Slang& self, vuk::PipelineBaseCreateInfo& pipeline_ci, const CompileInfo& compile_info) {
  ZoneScoped;

  if (!self.slang_session.has_value()) {
    OX_LOG_ERROR("A valid Slang session is needed!");
    return;
  }

  const auto root_directory = fs::get_directory(compile_info.path);
  const auto module_name = fs::get_file_name(compile_info.path);

  auto slang_module = self.slang_session->load_module({
      .path = compile_info.path,
      .module_name = module_name,
  });

  for (auto& v : compile_info.entry_points) {
    auto entry_point = slang_module->get_entry_point(v);
    if (!entry_point.has_value()) {
      OX_LOG_FATAL("Shader stage '{}' is not found for shader module '{}'", v, module_name);
      return;
    }

    pipeline_ci.add_spirv(entry_point->ir, module_name, v);
  }
}

void Slang::create_pipeline(this Slang& self,
                            vuk::Runtime& runtime,
                            const vuk::Name& name,
                            const option<vuk::DescriptorSetLayoutCreateInfo>& dci,
                            const CompileInfo& compile_info) {
  OX_CHECK_GT(compile_info.entry_points.size(), 0ul);

  vuk::PipelineBaseCreateInfo pipeline_ci = {};
  if (dci.has_value())
    pipeline_ci.explicit_set_layouts.emplace_back(*dci);

  self.add_shader(pipeline_ci, compile_info);

  TRY(runtime.create_named_pipeline(name, pipeline_ci))
}

} // namespace ox
