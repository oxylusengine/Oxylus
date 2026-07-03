#include "ShaderSession.hpp"

#include <fmt/std.h>
#include <spirv-tools/optimizer.hpp>

#include "OS/File.hpp"

namespace ox::rc {
auto ShaderSession::compile_shader(this ShaderSession& self, const ShaderCompileInfo& info)
  -> option<std::vector<ShaderEntryPointData>> {
  auto diagnostics_blob = Slang::ComPtr<slang::IBlob>();
  auto shader_path = (self.root_directory / info.path).lexically_normal();
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

  auto results = std::vector<ShaderEntryPointData>{};

  // Auto-discover entry points if none specified
  auto entry_point_names = std::vector<std::string>{};
  if (info.entry_points.empty()) {
    auto count = main_module->getDefinedEntryPointCount();
    for (i32 i = 0; i < count; i++) {
      auto ep = Slang::ComPtr<slang::IEntryPoint>();
      main_module->getDefinedEntryPoint(i, ep.writeRef());
      entry_point_names.emplace_back(ep->getFunctionReflection()->getName());
    }
  } else {
    entry_point_names = info.entry_points;
  }

  for (const auto& entry_point_name : entry_point_names) {
    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(main_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      if (diagnostics_blob) {
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
      }
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
    auto stage = static_cast<u32>(entry_point_reflection->getStage());

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

#if 1
    // SPIR-V optimization
    auto spv_message_cb =
      [&](spv_message_level_t level, const char* source, const spv_position_t& /*position*/, const char* message) {
        switch (level) {
          case SPV_MSG_FATAL:
          case SPV_MSG_INTERNAL_ERROR:
          case SPV_MSG_ERROR         : self.rc_session.push_error(fmt::format("[SPVOPT]: {}: {}", source, message)); break;
          case SPV_MSG_WARNING       :
          case SPV_MSG_INFO          :
          case SPV_MSG_DEBUG         : self.rc_session.push_message(fmt::format("[SPVOPT]: {}: {}", source, message)); break;
        }
      };

#if 0
    auto optimizer = spvtools::Optimizer(SPV_ENV_UNIVERSAL_1_5);
    optimizer.SetMessageConsumer(spv_message_cb);

    optimizer.RegisterPass(spvtools::CreateStripDebugInfoPass());
    optimizer.RegisterPass(spvtools::CreateStripNonSemanticInfoPass());

    optimizer.RegisterPass(spvtools::CreatePropagateLineInfoPass());
    optimizer.RegisterPass(spvtools::CreateWrapOpKillPass());
    optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
    optimizer.RegisterPass(spvtools::CreateMergeReturnPass());
    optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreatePrivateToLocalPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateScalarReplacementPass());
    optimizer.RegisterPass(spvtools::CreateLocalAccessChainConvertPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
    optimizer.RegisterPass(spvtools::CreateLocalMultiStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateCCPPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateLoopUnrollPass(true));
    optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
    optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass());
    optimizer.RegisterPass(spvtools::CreateCombineAccessChainsPass());
    optimizer.RegisterPass(spvtools::CreateSimplificationPass());
    optimizer.RegisterPass(spvtools::CreateScalarReplacementPass());
    optimizer.RegisterPass(spvtools::CreateLocalAccessChainConvertPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateSSARewritePass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateVectorDCEPass());
    optimizer.RegisterPass(spvtools::CreateDeadInsertElimPass());
    optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
    optimizer.RegisterPass(spvtools::CreateSimplificationPass());
    optimizer.RegisterPass(spvtools::CreateIfConversionPass());
    optimizer.RegisterPass(spvtools::CreateCopyPropagateArraysPass());
    optimizer.RegisterPass(spvtools::CreateReduceLoadSizePass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateBlockMergePass());
    optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass());
    optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
    optimizer.RegisterPass(spvtools::CreateBlockMergePass());
    optimizer.RegisterPass(spvtools::CreateSimplificationPass());
    optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
    optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass());
    optimizer.RegisterPass(spvtools::CreateCFGCleanupPass());
    optimizer.RegisterPass(spvtools::CreateRedundantLineInfoElimPass());

    auto optimizer_options = spvtools::OptimizerOptions{};
    optimizer_options.set_run_validator(false);

    auto spirv = std::vector<u32>{};
    OX_ASSERT(optimizer.Run(
      reinterpret_cast<const u32*>(spirv_code->getBufferPointer()),
      spirv_code->getBufferSize() / sizeof(u32),
      &spirv,
      optimizer_options
    ));
#else
    auto spirv = std::vector<u32>(spirv_code->getBufferSize() / sizeof(u32));
    std::memcpy(spirv.data(), spirv_code->getBufferPointer(), ox::size_bytes(spirv));
#endif

    results.push_back({
      .name = entry_point_name,
      .shader_stage = stage,
      .spirv = std::move(spirv),
    });
  }

  return results;
}

} // namespace ox::rc
