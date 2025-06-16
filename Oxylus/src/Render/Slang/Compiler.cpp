#include "Compiler.hpp"

#include <slang-com-ptr.h>
#include <slang.h>

#include "Core/FileSystem.hpp"
#include "Memory/Stack.hpp"

namespace ox {
struct SlangBlob : ISlangBlob {
  std::vector<u8> m_data = {};
  std::atomic_uint32_t m_refCount = 1;

  ISlangUnknown* getInterface(const SlangUUID&) { return nullptr; }
  SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) SLANG_OVERRIDE {
    ISlangUnknown* intf = getInterface(uuid);
    if (intf) {
      addRef();
      *outObject = intf;
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++m_refCount; }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
    --m_refCount;
    if (m_refCount == 0) {
      delete this;
      return 0;
    }
    return m_refCount;
  }

  SlangBlob(const std::vector<u8>& data) : m_data(data) {}
  virtual ~SlangBlob() = default;
  SLANG_NO_THROW const void* SLANG_MCALL getBufferPointer() final { return m_data.data(); };
  SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() final { return m_data.size(); };
};

// PERF: When we are at Editor environment, shaders obviously needs to be loaded
// through file system. But when we are at runtime environment, we don't need
// file system because we probably would have proper asset manager with all
// shaders are preloaded into virtual environment, so ::loadFile would just
// return already existing shader file.
struct SlangVirtualFS : ISlangFileSystem {
  std::string _root_dir;
  std::atomic_uint32_t m_refCount;

  SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) SLANG_OVERRIDE {
    ISlangUnknown* intf = getInterface(uuid);
    if (intf) {
      addRef();
      *outObject = intf;
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++m_refCount; }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
    --m_refCount;
    if (m_refCount == 0) {
      delete this;
      return 0;
    }
    return m_refCount;
  }

  SlangVirtualFS(std::string root_dir) : _root_dir(std::move(root_dir)), m_refCount(1) {}
  virtual ~SlangVirtualFS() = default;

  ISlangUnknown* getInterface(const SlangUUID&) { return nullptr; }
  SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID&) final { return nullptr; }

  SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* path_cstr, ISlangBlob** outBlob) final {
    const auto path = std::string(path_cstr);

    const auto root_path = std::filesystem::relative(_root_dir);
    const auto module_path = root_path / path;

    const auto result = fs::read_file(module_path.string());
    if (!result.empty()) {
      *outBlob = new SlangBlob(std::vector<u8>{result.data(), (result.data() + result.size())});

      OX_LOG_INFO("New shader module '{}' is loaded.", module_path.string());
      return SLANG_OK;
    }

    return SLANG_E_NOT_FOUND;
  }
};

using SlangSession_H = Handle<struct SlangSession>;
template <>
struct Handle<SlangModule>::Impl {
  SlangSession_H session = {};
  slang::IModule* slang_module = nullptr;
};

template <>
struct Handle<SlangSession>::Impl {
  std::unique_ptr<SlangVirtualFS> shader_virtual_env;
  Slang::ComPtr<slang::ISession> session;
};

template <>
struct Handle<SlangCompiler>::Impl {
  Slang::ComPtr<slang::IGlobalSession> global_session;
};

auto SlangModule::destroy() -> void {
  delete impl;
  impl = nullptr;
}

auto SlangModule::get_entry_point(std::string_view name) -> option<SlangEntryPoint> {
  ZoneScoped;
  memory::ScopedStack stack;

  Slang::ComPtr<slang::IEntryPoint> entry_point;
  if (SLANG_FAILED(impl->slang_module->findEntryPointByName(stack.null_terminate_cstr(name), entry_point.writeRef()))) {
    OX_LOG_ERROR("Shader entry point '{}' is not found.", name);
    return {};
  }

  std::vector<slang::IComponentType*> component_types;
  component_types.push_back(impl->slang_module);
  component_types.push_back(entry_point);

  Slang::ComPtr<slang::IComponentType> composed_program;
  {
    Slang::ComPtr<slang::IBlob> diagnostics_blob;
    const auto result = impl->session->session->createCompositeComponentType(
        component_types.data(), u32(component_types.size()), composed_program.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      OX_LOG_INFO("{}", (const char*)diagnostics_blob->getBufferPointer());
    }

    if (SLANG_FAILED(result)) {
      OX_LOG_ERROR("Failed to composite shader module.");
      return nullopt;
    }
  }

  Slang::ComPtr<slang::IComponentType> linked_program;
  {
    Slang::ComPtr<slang::IBlob> diagnostics_blob;
    composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      OX_LOG_INFO("{}", (const char*)diagnostics_blob->getBufferPointer());
    }
  }

  Slang::ComPtr<slang::IBlob> spirv_code;
  {
    Slang::ComPtr<slang::IBlob> diagnostics_blob;
    const auto result = linked_program->getEntryPointCode(0, 0, spirv_code.writeRef(), diagnostics_blob.writeRef());
    if (diagnostics_blob) {
      OX_LOG_INFO("{}", (const char*)diagnostics_blob->getBufferPointer());
    }

    if (SLANG_FAILED(result)) {
      OX_LOG_ERROR("Failed to compile shader module.\n{}", (const char*)diagnostics_blob->getBufferPointer());
      return nullopt;
    }
  }

  auto ir = std::vector<u32>(spirv_code->getBufferSize() / 4);
  std::memcpy(ir.data(), spirv_code->getBufferPointer(), spirv_code->getBufferSize());

  return SlangEntryPoint{
      .ir = std::move(ir),
  };
}

ShaderReflection SlangModule::get_reflection() {
  ZoneScoped;

  ShaderReflection result = {};
  slang::ShaderReflection* program_layout = impl->slang_module->getLayout();
  option<u32> compute_entry_point_index = nullopt;

  const uint64_t entry_point_count = program_layout->getEntryPointCount();
  for (u32 i = 0; i < entry_point_count; i++) {
    auto* entry_point = program_layout->getEntryPointByIndex(i);
    if (entry_point->getStage() == SLANG_STAGE_COMPUTE) {
      compute_entry_point_index = i;
      break;
    }
  }

  // Get push constants
  const u32 param_count = program_layout->getParameterCount();
  for (u32 i = 0; i < param_count; i++) {
    auto* param = program_layout->getParameterByIndex(i);
    auto* type_layout = param->getTypeLayout();
    auto* element_type_layout = type_layout->getElementTypeLayout();
    const auto param_category = param->getCategory();

    if (param_category == slang::ParameterCategory::PushConstantBuffer) {
      usize push_constant_size = 0;
      const auto field_count = element_type_layout->getFieldCount();
      for (u32 f = 0; f < field_count; f++) {
        auto* field_param = element_type_layout->getFieldByIndex(f);
        auto* field_type_layout = field_param->getTypeLayout();
        push_constant_size += field_type_layout->getSize();
      }

      result.pipeline_layout_index = static_cast<u32>(push_constant_size / sizeof(u32));
      break;
    }
  }

  if (compute_entry_point_index.has_value()) {
    auto* entry_point = program_layout->getEntryPointByIndex(compute_entry_point_index.value());
    entry_point->getComputeThreadGroupSize(3, glm::value_ptr(result.thread_group_size));
  }

  return result;
}

auto SlangModule::session() -> SlangSession { return impl->session; }

auto SlangSession::destroy() -> void {
  delete impl;
  impl = nullptr;
}

auto SlangSession::load_module(const SlangModuleInfo& info) -> option<SlangModule> {
  ZoneScoped;
  memory::ScopedStack stack;

  slang::IModule* slang_module = {};
  const auto& path_str = info.path;
  const auto source_data = fs::read_file(info.path);
  if (source_data.empty()) {
    OX_LOG_ERROR("Failed to read shader file '{}'!", path_str.c_str());
    return nullopt;
  }

  Slang::ComPtr<slang::IBlob> diagnostics_blob;
  slang_module = impl->session->loadModuleFromSourceString(
      info.module_name.c_str(), path_str.c_str(), source_data.c_str(), diagnostics_blob.writeRef());

  if (diagnostics_blob) {
    OX_LOG_INFO("{}", (const char*)diagnostics_blob->getBufferPointer());
  }

  const auto module_impl = new SlangModule::Impl;
  module_impl->slang_module = slang_module;
  module_impl->session = impl;

  return SlangModule(module_impl);
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

  auto slang_fs = std::make_unique<SlangVirtualFS>(info.root_directory);

  slang::CompilerOptionEntry entries[] = {
      {.name = slang::CompilerOptionName::Optimization,
       .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_OPTIMIZATION_LEVEL_MAXIMAL}},
#if OX_DEBUG
      {.name = slang::CompilerOptionName::DebugInformationFormat,
       .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_FORMAT_C7}},
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
       .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "vk_mem_model"}}};
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

  const auto search_path = info.root_directory;
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
      .fileSystem = slang_fs.get(),
  };
  Slang::ComPtr<slang::ISession> session;
  if (SLANG_FAILED(impl->global_session->createSession(session_desc, session.writeRef()))) {
    OX_LOG_ERROR("Failed to create compiler session!");
    return nullopt;
  }

  const auto session_impl = new SlangSession::Impl;
  session_impl->shader_virtual_env = std::move(slang_fs);
  session_impl->session = std::move(session);

  return SlangSession(session_impl);
}

} // namespace ox
