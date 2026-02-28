#include "Render/Slang/Compiler.hpp"

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>
#include <shared_mutex>
#include <slang-com-ptr.h>
#include <slang.h>
#include <spirv-tools/optimizer.hpp>

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
template <>
struct Handle<SlangSession>::Impl {
  std::string name = {};
  std::filesystem::path root_directory = {};
  std::shared_mutex cached_modules_mutex = {};
  ankerl::unordered_dense::map<std::filesystem::path, slang::IModule*> cached_modules = {};
  Slang::ComPtr<slang::ISession> slang_session = {};
};

template <>
struct Handle<SlangCompiler>::Impl {
  Slang::ComPtr<slang::IGlobalSession> global_session;
};

auto SlangSession::destroy() -> void {
  delete impl;
  impl = nullptr;
}

auto SlangSession::compile_shader(const SlangShaderInfo& info) -> option<std::vector<SlangEntryPoint>> {
  ZoneScoped;
  auto diagnostics_blob = Slang::ComPtr<slang::IBlob>();
  auto shader_path = (impl->root_directory / info.path).lexically_normal();
  auto shader_path_str = shader_path.string();

  slang::IModule* main_module = nullptr;
  {
    auto read_lock = std::shared_lock(impl->cached_modules_mutex);
    auto slang_module_it = impl->cached_modules.find(shader_path);
    if (slang_module_it == impl->cached_modules.end()) {
      read_lock.unlock();

      const auto source_data = File::to_string(shader_path);
      if (source_data.empty()) {
        OX_LOG_ERROR(
          "An error occured during compiling '{}::{}', the file '{}' is empty.",
          impl->name,
          info.module_name,
          shader_path
        );
        return nullopt;
      }

      main_module = impl->slang_session->loadModuleFromSourceString(
        info.module_name.c_str(),
        shader_path_str.c_str(),
        source_data.c_str(),
        diagnostics_blob.writeRef()
      );

      auto write_lock = std::unique_lock(impl->cached_modules_mutex);
      impl->cached_modules.emplace(shader_path, main_module);
    } else {
      main_module = slang_module_it->second;
    }
  }

  if (diagnostics_blob) {
    auto sv = std::string_view(
      static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
      diagnostics_blob->getBufferSize()
    );
    OX_LOG_INFO("{}::{} {}", impl->name, info.module_name, sv);
  }

  auto entry_points = std::vector<SlangEntryPoint>{};
  for (const auto& entry_point_name : info.entry_points) {
    auto entry_point = Slang::ComPtr<slang::IEntryPoint>();
    if (SLANG_FAILED(main_module->findEntryPointByName(entry_point_name.c_str(), entry_point.writeRef()))) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      OX_LOG_ERROR(
        "An error occured while compiling entry point {}::{}::{} {}",
        impl->name,
        info.module_name,
        entry_point_name,
        sv
      );
      return nullopt;
    }

    // Composition
    auto component_types = std::vector<slang::IComponentType*>();
    component_types.push_back(main_module);
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
      OX_LOG_ERROR("[Slang Composer] {}::{}::{} {}", impl->name, info.module_name, entry_point_name, sv);
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
      OX_LOG_ERROR("[Slang Linker] {}::{}::{} {}", impl->name, info.module_name, entry_point_name, sv);
    }
    if (SLANG_FAILED(link_result)) {
      return nullopt;
    }

    // Reflection
    auto* entry_point_layout = linked_program->getLayout();
    auto* entry_point_reflection = entry_point_layout->getEntryPointByIndex(0);

    // Codegen
    auto spirv_code = Slang::ComPtr<slang::IBlob>();
    auto codegen_result = linked_program->getEntryPointCode(0, 0, spirv_code.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      auto sv = std::string_view(
        static_cast<const c8*>(diagnostics_blob->getBufferPointer()),
        diagnostics_blob->getBufferSize()
      );
      OX_LOG_ERROR("[Slang Codegen] {}::{}::{} {}", impl->name, info.module_name, entry_point_name, sv);
    }
    if (SLANG_FAILED(codegen_result)) {
      return nullopt;
    }

#if 1
    auto spv_message_cb =
      [&](spv_message_level_t level, const char* source, const spv_position_t& position, const char* message) {
        switch (level) {
          case SPV_MSG_FATAL:
          case SPV_MSG_INTERNAL_ERROR:
          case SPV_MSG_ERROR         : {
            OX_LOG_ERROR("[SPVOPT]: {}: {}", source, message);
          } break;
          case SPV_MSG_WARNING: {
            OX_LOG_WARN("[SPVOPT]: {}: {}", source, message);
          } break;
          case SPV_MSG_INFO: {
            OX_LOG_INFO("[SPVOPT]: {}: {}", source, message);
          } break;
          case SPV_MSG_DEBUG: {
            OX_LOG_TRACE("[SPVOPT]: {}: {}", source, message);
          } break;
        }
      };

    auto optimizer = spvtools::Optimizer(SPV_ENV_UNIVERSAL_1_5);
    optimizer.SetMessageConsumer(spv_message_cb);

    // Order of these passes matter, also there is a reason some of them are duplicate

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

    entry_points.push_back({.code = std::move(spirv)});
  }

  return std::move(entry_points);
}

auto SlangCompiler::create() -> option<SlangCompiler> {
  ZoneScoped;

  const auto impl = new Impl;
  slang::createGlobalSession(impl->global_session.writeRef());
  return SlangCompiler(impl);
}

auto SlangCompiler::destroy() -> void {
  delete impl;
  impl = nullptr;
}

auto SlangCompiler::new_session(const SlangSessionInfo& info) -> option<SlangSession> {
  ZoneScoped;

  slang::CompilerOptionEntry entries[] = {
    {.name = slang::CompilerOptionName::Optimization,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_OPTIMIZATION_LEVEL_MAXIMAL}},
#if 0
    {.name = slang::CompilerOptionName::DebugInformationFormat,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_FORMAT_DEFAULT}},
    {.name = slang::CompilerOptionName::DebugInformation,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_LEVEL_MAXIMAL}},
#endif
    {.name = slang::CompilerOptionName::UseUpToDateBinaryModule,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    {.name = slang::CompilerOptionName::GLSLForceScalarLayout,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    {.name = slang::CompilerOptionName::Language,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "slang"}},
    {.name = slang::CompilerOptionName::VulkanUseEntryPointName,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    {.name = slang::CompilerOptionName::DisableWarning,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "39001"}},
    {.name = slang::CompilerOptionName::DisableWarning,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "41012"}},
    {.name = slang::CompilerOptionName::DisableWarning,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "41017"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "vk_mem_model"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvGroupNonUniformBallot"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvGroupNonUniformShuffle"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvImageGatherExtended"}}
  };

  std::vector<slang::PreprocessorMacroDesc> macros;
  macros.reserve(info.definitions.size());
  for (const auto& [first, second] : info.definitions) {
    macros.emplace_back(first.c_str(), second.c_str());
  }

  slang::TargetDesc target_desc = {
    .format = SLANG_SPIRV,
    .profile = impl->global_session->findProfile("spirv_1_5"),
    .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY,
    .floatingPointMode = SLANG_FLOATING_POINT_MODE_FAST,
    .lineDirectiveMode = SLANG_LINE_DIRECTIVE_MODE_STANDARD,
    .forceGLSLScalarBufferLayout = true,
    .compilerOptionEntries = entries,
    .compilerOptionEntryCount = static_cast<u32>(count_of(entries)),
  };

  const auto search_path = info.root_directory.string();
  const auto* search_path_cstr = search_path.c_str();
  const c8* search_paths[] = {search_path_cstr};
  const slang::SessionDesc session_desc = {
    .targets = &target_desc,
    .targetCount = 1,
    .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
    .searchPaths = search_paths,
    .searchPathCount = count_of(search_paths),
    .preprocessorMacros = macros.data(),
    .preprocessorMacroCount = static_cast<u32>(macros.size()),
  };
  Slang::ComPtr<slang::ISession> session;
  if (SLANG_FAILED(impl->global_session->createSession(session_desc, session.writeRef()))) {
    OX_LOG_ERROR("Failed to create compiler session!");
    return nullopt;
  }

  const auto session_impl = new SlangSession::Impl;
  session_impl->name = info.name;
  session_impl->root_directory = info.root_directory;
  session_impl->slang_session = std::move(session);

  return SlangSession(session_impl);
}
} // namespace ox
