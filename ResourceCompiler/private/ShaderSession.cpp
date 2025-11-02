#include "ShaderSession.hpp"

#include <OS/File.hpp>
#include <fmt/format.h>
#include <slang-com-ptr.h>
#include <slang.h>
#include <span>

#include "ResourceCompiler.hpp"

namespace ox::rc {
auto ShaderSession::compile_shader(const ShaderInfo& info) -> AssetID {
  auto diagnostics_blob = Slang::ComPtr<slang::IBlob>();
  const auto& path_str = info.path.string();
  const auto source_data = File::to_string(path_str);
  if (source_data.empty()) {
    impl->rc_session.push_error(
      fmt::format("An error occured during compiling '{}::{}', the file is empty.", impl->name, info.module_name)
    );
    return AssetID::Invalid;
  }

  auto* slang_module = impl->slang_session->loadModuleFromSourceString(
    info.module_name.c_str(),
    path_str.c_str(),
    source_data.c_str(),
    diagnostics_blob.writeRef()
  );

  if (diagnostics_blob) {
    auto sv = std::string_view(
      static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
      diagnostics_blob->getBufferSize()
    );
    impl->rc_session.push_message(fmt::format("{}::{} {}", impl->name, info.module_name, sv));
  }

  auto slang_stage_to_entry_kind = [](SlangStage slang_stage) {
    switch (slang_stage) {
      case SLANG_STAGE_VERTEX  : return ShaderAsset::EntryPointKind::Vertex;
      case SLANG_STAGE_FRAGMENT: return ShaderAsset::EntryPointKind::Fragment;
      case SLANG_STAGE_COMPUTE : return ShaderAsset::EntryPointKind::Compute;
      case SLANG_STAGE_COUNT   :
      default                  : return ShaderAsset::EntryPointKind::Count;
    }
  };

  auto shader_asset = ShaderAsset{};
  auto asset_data = std::vector<u8>();
  for (const auto& entry_point_name : info.entry_points) {
    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(slang_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      impl->rc_session.push_error(
        fmt::format(
          "An error occured while compiling entry point {}::{}::{} {}",
          impl->name,
          info.module_name,
          entry_point_name,
          sv
        )
      );
      return AssetID::Invalid;
    }

    // Composition
    auto component_types = std::vector<slang::IComponentType*>();
    component_types.push_back(slang_module);
    component_types.push_back(entry_point);
    auto composed_program = Slang::ComPtr<slang::IComponentType>();
    auto compose_result = impl->slang_session->createCompositeComponentType(
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
      impl->rc_session.push_message(
        fmt::format("[Slang Composer] {}::{}::{} {}", impl->name, info.module_name, entry_point_name, sv)
      );
    }
    if (SLANG_FAILED(compose_result)) {
      return AssetID::Invalid;
    }

    // Linking
    auto linked_program = Slang::ComPtr<slang::IComponentType>();
    auto link_result = composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      impl->rc_session.push_message(
        fmt::format("[Slang Linker] {}::{}::{} {}", impl->name, info.module_name, entry_point_name, sv)
      );
    }
    if (SLANG_FAILED(link_result)) {
      return AssetID::Invalid;
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
      impl->rc_session.push_message(
        fmt::format("[Slang Codegen] {}::{}::{} {}", impl->name, info.module_name, entry_point_name, sv)
      );
    }
    if (SLANG_FAILED(codegen_result)) {
      return AssetID::Invalid;
    }

    auto& [name_offset, name_length] = shader_asset.entry_point_names[entry_point_kind];
    name_offset = asset_data.size();
    name_length = entry_point_name.length();
    asset_data.insert(asset_data.end(), entry_point_name.begin(), entry_point_name.end());
    asset_data.insert(asset_data.end(), '0'); // just to be safe

    auto spirv = std::span(reinterpret_cast<const u8*>(spirv_code->getBufferPointer()), spirv_code->getBufferSize());
    auto& [code_offset, code_length] = shader_asset.entry_point_ranges[entry_point_kind];
    code_offset = asset_data.size();
    code_length = spirv.size();

    asset_data.insert(asset_data.end(), spirv.begin(), spirv.end());
  }

  auto asset_id = impl->rc_session.create_asset(AssetType::Shader);
  impl->rc_session.set_asset_info(asset_id, shader_asset);
  impl->rc_session.set_asset_data(asset_id, std::move(asset_data));

  return asset_id;
}

} // namespace ox::rc
