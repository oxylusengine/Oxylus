#include "ShaderSession.hpp"

#include <fmt/format.h>
#include <slang-com-ptr.h>
#include <slang.h>
#include <span>

#include "Core/FileSystem.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
auto ShaderSession::compile_shader(const ShaderInfo& info) -> std::expected<AssetID, Error> {
  auto diagnostics_blob = Slang::ComPtr<slang::IBlob>();
  const auto& path_str = info.path.string();
  const auto source_data = fs::read_file(path_str); // why is this not taking a fs::path???
  if (source_data.empty()) {
    return std::unexpected(Error::ShaderModuleCompilation);
  }

  auto* slang_module = impl->slang_session->loadModuleFromSourceString(
    info.module_name.c_str(),
    path_str.c_str(),
    source_data.c_str(),
    diagnostics_blob.writeRef()
  );

  if (diagnostics_blob) {
    impl->diagnostic_messages.push_back(
      std::string(static_cast<const c8*>(diagnostics_blob->getBufferPointer()), diagnostics_blob->getBufferSize())
    );
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

  auto entry_point_data = std::vector<u8>();
  auto entry_point_ranges = std::array<std::pair<u32, u32>, ShaderAsset::EntryPointKind::Count>();
  for (const auto& entry_point_name : info.entry_points) {
    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(slang_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      auto msg = fmt::format("Shader entry point '{}:{}' is not found.", info.module_name, entry_point_name);
      impl->diagnostic_messages.emplace_back(std::move(msg));
      return std::unexpected(Error::ShaderEntryPointCompilation);
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
      auto diag_msg = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      auto msg = fmt::format("Slang Composer: [{}] {}", info.module_name, diag_msg);
      impl->diagnostic_messages.push_back(std::move(msg));
    }
    if (SLANG_FAILED(compose_result)) {
      return std::unexpected(Error::ShaderEntryPointComposer);
    }

    // Linking
    auto linked_program = Slang::ComPtr<slang::IComponentType>();
    auto link_result = composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto diag_msg = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      auto msg = fmt::format("Slang Linker: [{}] {}", info.module_name, diag_msg);
      impl->diagnostic_messages.push_back(std::move(msg));
    }
    if (SLANG_FAILED(link_result)) {
      return std::unexpected(Error::ShaderEntryPointLinker);
    }

    // Reflection
    auto* entry_point_layout = linked_program->getLayout();
    auto* entry_point_reflection = entry_point_layout->getEntryPointByIndex(0);
    auto entry_point_kind = slang_stage_to_entry_kind(entry_point_reflection->getStage());

    // Codegen
    auto spirv_code = Slang::ComPtr<slang::IBlob>();
    auto codegen_result = linked_program->getEntryPointCode(0, 0, spirv_code.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto diag_msg = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      auto msg = fmt::format("Slang Codegen: [{}] {}", info.module_name, diag_msg);
      impl->diagnostic_messages.push_back(std::move(msg));
    }
    if (SLANG_FAILED(codegen_result)) {
      return std::unexpected(Error::ShaderEntryPointCodegen);
    }

    auto spirv = std::span(reinterpret_cast<const u8*>(spirv_code->getBufferPointer()), spirv_code->getBufferSize());
    entry_point_ranges[entry_point_kind] = {entry_point_data.size() / sizeof(u32), spirv.size() * sizeof(u32)};
    entry_point_data.insert(entry_point_data.end(), spirv.begin(), spirv.end());
  }

  auto asset_id = impl->rc_session.create_asset(AssetType::Shader);
}
} // namespace ox::rc
