#include "ShaderSession.hpp"

#include <fmt/std.h>
#include <spirv-tools/optimizer.hpp>

#include "OS/File.hpp"

namespace ox::rc {
static auto blob_to_sv(slang::IBlob* blob) -> std::string_view {
  return {
    static_cast<const c8*>(blob->getBufferPointer()),
    blob->getBufferSize(),
  };
}

auto ShaderSession::compile_shader(this ShaderSession& self, const ShaderCompileInfo& info)
  -> option<std::vector<ShaderEntryPointData>> {
  const auto shader_path = (self.root_directory / info.path).lexically_normal();
  const auto shader_path_str = shader_path.string();
  const auto prefix = fmt::format("{}::{}", self.name, info.module_name);

  slang::IModule* main_module = nullptr;
  {
    auto read_lock = std::shared_lock(self.cached_modules_mutex);
    const auto slang_module_it = self.cached_modules.find(shader_path);
    if (slang_module_it == self.cached_modules.end()) {
      read_lock.unlock();

      const auto source_data = File::to_string(shader_path);
      if (source_data.empty()) {
        self.rc_session.push_error(fmt::format("{}: file '{}' is empty.", prefix, shader_path));
        return nullopt;
      }

      auto diag = Slang::ComPtr<slang::IBlob>();
      main_module = self.slang_session->loadModuleFromSourceString(
        info.module_name.c_str(),
        shader_path_str.c_str(),
        source_data.c_str(),
        diag.writeRef()
      );

      if (diag) {
        self.rc_session.push_error(fmt::format("{}: {}", prefix, blob_to_sv(diag)));
      }

      if (!main_module) {
        if (!diag) {
          self.rc_session.push_error(fmt::format("{}: failed to load module (no diagnostics).", prefix));
        }
        return nullopt;
      }

      auto write_lock = std::unique_lock(self.cached_modules_mutex);
      self.cached_modules.emplace(shader_path, main_module);
    } else {
      main_module = slang_module_it->second;
    }
  }

  auto results = std::vector<ShaderEntryPointData>{};

  auto entry_point_names = std::vector<std::string>{};
  if (info.entry_points.empty()) {
    const auto count = main_module->getDefinedEntryPointCount();
    for (i32 i = 0; i < count; i++) {
      auto ep = Slang::ComPtr<slang::IEntryPoint>();
      main_module->getDefinedEntryPoint(i, ep.writeRef());
      entry_point_names.emplace_back(ep->getFunctionReflection()->getName());
    }
  } else {
    entry_point_names = info.entry_points;
  }

  for (const auto& entry_point_name : entry_point_names) {
    const auto ep_prefix = fmt::format("{}::{}", prefix, entry_point_name);

    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(main_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      self.rc_session.push_error(fmt::format("{}: entry point not found.", ep_prefix));
      return nullopt;
    }

    // Composition
    auto component_types = std::vector<slang::IComponentType*>();
    component_types.push_back(main_module);
    component_types.push_back(entry_point);
    auto composed_program = Slang::ComPtr<slang::IComponentType>();
    auto compose_diag = Slang::ComPtr<slang::IBlob>();
    const auto compose_result = self.slang_session->createCompositeComponentType(
      component_types.data(),
      component_types.size(),
      composed_program.writeRef(),
      compose_diag.writeRef()
    );
    if (compose_diag) {
      self.rc_session.push_message(fmt::format("[Composer] {}: {}", ep_prefix, blob_to_sv(compose_diag)));
    }
    if (SLANG_FAILED(compose_result)) {
      if (!compose_diag) {
        self.rc_session.push_error(fmt::format("{}: composition failed (no diagnostics).", ep_prefix));
      }
      return nullopt;
    }

    // Linking
    auto linked_program = Slang::ComPtr<slang::IComponentType>();
    auto link_diag = Slang::ComPtr<slang::IBlob>();
    const auto link_result = composed_program->link(linked_program.writeRef(), link_diag.writeRef());
    if (link_diag) {
      self.rc_session.push_message(fmt::format("[Linker] {}: {}", ep_prefix, blob_to_sv(link_diag)));
    }
    if (SLANG_FAILED(link_result)) {
      if (!link_diag) {
        self.rc_session.push_error(fmt::format("{}: linking failed (no diagnostics).", ep_prefix));
      }
      return nullopt;
    }

    // Reflection
    auto* entry_point_layout = linked_program->getLayout();
    auto* entry_point_reflection = entry_point_layout->getEntryPointByIndex(0);
    const auto stage = static_cast<u32>(entry_point_reflection->getStage());

    // Codegen
    auto spirv_code = Slang::ComPtr<slang::IBlob>();
    auto codegen_diag = Slang::ComPtr<slang::IBlob>();
    const auto codegen_result =
      linked_program->getEntryPointCode(0, 0, spirv_code.writeRef(), codegen_diag.writeRef());
    if (codegen_diag) {
      self.rc_session.push_message(fmt::format("[Codegen] {}: {}", ep_prefix, blob_to_sv(codegen_diag)));
    }
    if (SLANG_FAILED(codegen_result)) {
      if (!codegen_diag) {
        self.rc_session.push_error(fmt::format("{}: codegen failed (no diagnostics).", ep_prefix));
      }
      return nullopt;
    }

    auto spirv = std::vector<u32>(spirv_code->getBufferSize() / sizeof(u32));
    std::memcpy(spirv.data(), spirv_code->getBufferPointer(), ox::size_bytes(spirv));

    results.push_back({
      .name = entry_point_name,
      .shader_stage = stage,
      .spirv = std::move(spirv),
    });
  }

  return results;
}

} // namespace ox::rc
