#include "ShaderSession.hpp"

#include <fmt/format.h>
#include <slang-com-ptr.h>
#include <slang.h>

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

  for (const auto& entry_point_name : info.entry_points) {
    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(slang_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      auto msg = fmt::format("Shader entry point '{}' is not found.", entry_point_name);
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
      auto msg = fmt::format("Slang Composer: {}", diag_msg);
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
      auto msg = fmt::format("Slang Linker: {}", diag_msg);
      impl->diagnostic_messages.push_back(std::move(msg));
    }
    if (SLANG_FAILED(link_result)) {
      return std::unexpected(Error::ShaderEntryPointLinker);
    }

    // Codegen
    auto spirv_code = Slang::ComPtr<slang::IBlob>();
    auto codegen_result = linked_program->getEntryPointCode(0, 0, spirv_code.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto diag_msg = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      auto msg = fmt::format("Slang Codegen: {}", diag_msg);
      impl->diagnostic_messages.push_back(std::move(msg));
    }
    if (SLANG_FAILED(link_result)) {
      return std::unexpected(Error::ShaderEntryPointCodegen);
    }
  }
}
} // namespace ox::rc
