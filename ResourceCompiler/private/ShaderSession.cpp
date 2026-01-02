#include "ShaderSession.hpp"

#include <OS/File.hpp>
#include <fmt/format.h>
#include <fmt/std.h>
#include <slang-com-ptr.h>
#include <slang.h>
#include <span>

#include "Common.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
auto ShaderSession::compile_shader(this ShaderSession& self, const ShaderInfo& info)
  -> option<CompileResult<ShaderAssetEntry>> {
  auto diagnostics_blob = Slang::ComPtr<slang::IBlob>();
  auto shader_path = (self.root_dir / info.path).lexically_normal();
  auto shader_path_str = shader_path.string();

  slang::IModule* main_module = nullptr;
  {
    auto read_lock = std::shared_lock(self.cached_modules_mutex);
    auto slang_module_it = self.cached_modules.find(shader_path);
    if (slang_module_it == self.cached_modules.end()) {
      read_lock.unlock();

      const auto source_data = File::to_string(shader_path);
      if (source_data.empty()) {
        self.rc_session.push_error(
          fmt::format(
            "An error occured during compiling '{}::{}', the file '{}' is empty.",
            self.name,
            info.module_name,
            shader_path
          )
        );
        return nullopt;
      }

      main_module = self.slang_session->loadModuleFromSourceString(
        info.module_name.c_str(),
        shader_path_str.c_str(),
        source_data.c_str(),
        diagnostics_blob.writeRef()
      );

      auto write_lock = std::unique_lock(self.cached_modules_mutex);
      self.cached_modules.emplace(shader_path, main_module);
    } else {
      main_module = slang_module_it->second;
    }
  }

  if (diagnostics_blob) {
    auto sv = std::string_view(
      static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
      diagnostics_blob->getBufferSize()
    );
    self.rc_session.push_message(fmt::format("{}::{} {}", self.name, info.module_name, sv));
  }

  auto slang_stage_to_entry_kind = [](SlangStage slang_stage) {
    switch (slang_stage) {
      case SLANG_STAGE_VERTEX  : return ShaderAssetEntry::EntryPointKind::Vertex;
      case SLANG_STAGE_FRAGMENT: return ShaderAssetEntry::EntryPointKind::Fragment;
      case SLANG_STAGE_COMPUTE : return ShaderAssetEntry::EntryPointKind::Compute;
      case SLANG_STAGE_COUNT   :
      default                  : return ShaderAssetEntry::EntryPointKind::Count;
    }
  };

  auto asset_data = std::vector<u8>{};
  auto entry_points = std::vector<ShaderAssetEntry::EntryPoint>{};
  for (const auto& entry_point_name : info.entry_points) {
    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(main_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      self.rc_session.push_error(
        fmt::format(
          "An error occured while compiling entry point {}::{}::{} {}",
          self.name,
          info.module_name,
          entry_point_name,
          sv
        )
      );
      return nullopt;
    }

    // Composition
    auto component_types = std::vector<slang::IComponentType*>();
    component_types.push_back(main_module);
    component_types.push_back(entry_point);
    auto composed_program = Slang::ComPtr<slang::IComponentType>();
    auto compose_result = self.slang_session->createCompositeComponentType(
      component_types.data(),
      component_types.size(),
      composed_program.writeRef(),
      diagnostics_blob.writeRef()
    );
    if (diagnostics_blob) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      self.rc_session.push_message(
        fmt::format("[Slang Composer] {}::{}::{} {}", self.name, info.module_name, entry_point_name, sv)
      );
    }
    if (SLANG_FAILED(compose_result)) {
      return nullopt;
    }

    // Linking
    auto linked_program = Slang::ComPtr<slang::IComponentType>();
    auto link_result = composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      self.rc_session.push_message(
        fmt::format("[Slang Linker] {}::{}::{} {}", self.name, info.module_name, entry_point_name, sv)
      );
    }
    if (SLANG_FAILED(link_result)) {
      return nullopt;
    }

    // Reflection
    auto* entry_point_layout = linked_program->getLayout();
    auto* entry_point_reflection = entry_point_layout->getEntryPointByIndex(0);
    auto entry_point_kind = slang_stage_to_entry_kind(entry_point_reflection->getStage());

    // Codegen
    auto spirv_code = Slang::ComPtr<slang::IBlob>();
    auto codegen_result = linked_program->getEntryPointCode(0, 0, spirv_code.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      self.rc_session.push_message(
        fmt::format("[Slang Codegen] {}::{}::{} {}", self.name, info.module_name, entry_point_name, sv)
      );
    }
    if (SLANG_FAILED(codegen_result)) {
      return nullopt;
    }

    auto spirv = std::span(
      reinterpret_cast<const u32*>(spirv_code->getBufferPointer()),
      spirv_code->getBufferSize() / sizeof(u32)
    );
    entry_points.emplace_back(entry_point_kind, push_str(asset_data, entry_point_name), push_span(asset_data, spirv));
  }

  auto uuid = UUID::generate_random();
  auto hash = self.rc_session.hash_file(shader_path);
  auto asset_id = self.rc_session.create_asset(uuid, AssetType::Shader);
  auto shader_asset = ShaderAssetEntry{
    .entry_points = push_span(asset_data, std::span<const ShaderAssetEntry::EntryPoint>(entry_points))
  };

  return CompileResult{shader_asset, asset_data};
}

} // namespace ox::rc
