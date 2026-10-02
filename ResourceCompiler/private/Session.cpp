#include "Session.hpp"

#include <ranges>
#include <thread>
#include <tuple>
#include <utility>
#include <zpp_bits.h>

#include "ModelCompiler.hpp"
#include "OS/File.hpp"
#include "Parallel.hpp"
#include "ShaderSession.hpp"
#include "TextureCompiler.hpp"

namespace ox::rc {
struct ShaderTask {
  option<std::vector<ShaderEntryPointData>> result = nullopt;
  ShaderDiagnostics diag = {};
};

auto create_shader_session(slang::IGlobalSession* global_session, const ShaderSessionInfo& info)
  -> Slang::ComPtr<slang::ISession> {
  slang::CompilerOptionEntry entries[] = {
#if 1
    {.name = slang::CompilerOptionName::DebugInformationFormat,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_FORMAT_C7}},
    {.name = slang::CompilerOptionName::DebugInformation,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_LEVEL_MAXIMAL}},
#endif
    {.name = slang::CompilerOptionName::Optimization,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = info.optimization_level}},
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
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvImageGatherExtended"}},
  };

  std::vector<slang::PreprocessorMacroDesc> macros;
  macros.reserve(info.definitions.size());
  for (const auto& [first, second] : info.definitions) {
    macros.emplace_back(first.c_str(), second.c_str());
  }

  slang::TargetDesc target_desc = {
    .format = SLANG_SPIRV,
    .profile = global_session->findProfile("spirv_1_5"),
    .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY,
    .floatingPointMode = SLANG_FLOATING_POINT_MODE_FAST,
    .lineDirectiveMode = SLANG_LINE_DIRECTIVE_MODE_STANDARD,
    .forceGLSLScalarBufferLayout = true,
    .compilerOptionEntries = entries,
    .compilerOptionEntryCount = static_cast<u32>(count_of(entries)),
  };

  // `root_directory` first so a project's own shaders shadow same-named engine ones.
  std::vector<std::string> search_path_storage;
  search_path_storage.reserve(info.include_directories.size() + 1);
  search_path_storage.emplace_back(info.root_directory.string());
  for (const auto& include_directory : info.include_directories) {
    search_path_storage.emplace_back(include_directory.string());
  }

  std::vector<const c8*> search_paths;
  search_paths.reserve(search_path_storage.size());
  for (const auto& stored_path : search_path_storage) {
    search_paths.emplace_back(stored_path.c_str());
  }

  const slang::SessionDesc session_desc = {
    .targets = &target_desc,
    .targetCount = 1,
    .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
    .searchPaths = search_paths.data(),
    .searchPathCount = static_cast<SlangInt>(search_paths.size()),
    .preprocessorMacros = macros.data(),
    .preprocessorMacroCount = static_cast<u32>(macros.size()),
  };
  Slang::ComPtr<slang::ISession> session;
  if (SLANG_FAILED(global_session->createSession(session_desc, session.writeRef()))) {
    return nullptr;
  }

  return session;
}

auto Session::create(const SessionCreateInfo& info) -> option<Session> {
  auto* self = new Session::Impl;
  if (SLANG_FAILED(slang::createGlobalSession(self->slang_global_session.writeRef()))) {
    delete self;
    return nullopt;
  }

  auto thread_count = info.thread_count;
  if (thread_count == 0) {
    const auto available = std::thread::hardware_concurrency();
    thread_count = available > 1 ? available - 1 : 1_u32;
  }

  self->job_manager.set_thread_count(thread_count);
  if (!self->job_manager.init().has_value()) {
    std::ignore = self->job_manager.deinit();
    delete self;
    return nullopt;
  }

  return Session(self);
}

auto Session::destroy() -> void {
  // the workers park on a condition variable until `running` is cleared, so the pool has to be torn
  // down before the impl it lives in
  std::ignore = impl->job_manager.deinit();
  delete impl;
  impl = nullptr;
}

auto Session::add_request(const ShaderCompileRequest& request) -> void { impl->shader_requests.emplace_back(request); }

auto Session::push_error(std::string msg) -> void {
  auto lock = std::unique_lock(impl->messages_mutex);
  impl->errors.push_back(std::move(msg));
}

auto Session::push_message(std::string msg) -> void {
  auto lock = std::unique_lock(impl->messages_mutex);
  impl->messages.push_back(std::move(msg));
}

auto Session::get_errors() const -> std::vector<std::string> {
  auto lock = std::shared_lock(impl->messages_mutex);
  return impl->errors;
}

auto Session::get_messages() const -> std::vector<std::string> {
  auto lock = std::shared_lock(impl->messages_mutex);
  return impl->messages;
}

auto Session::take_diagnostics() -> SessionDiagnostics {
  auto lock = std::unique_lock(impl->messages_mutex);

  return {
    .errors = std::exchange(impl->errors, {}),
    .messages = std::exchange(impl->messages, {}),
  };
}

auto Session::compile() -> bool {
  bool success = true;

  for (const auto& request : impl->shader_requests) {
    auto slang_session = create_shader_session(impl->slang_global_session, request.session_info);
    if (!slang_session) {
      push_error(fmt::format("Failed to create shader session '{}'.", request.session_info.name));
      success = false;
      continue;
    }

    // one session shared by every worker, so the modules this request imports are parsed once
    auto shader_session = ShaderSession{
      .slang_session = slang_session,
      .name = request.session_info.name,
      .root_directory = request.session_info.root_directory,
    };

    // pre-sized and filled by index so the archive comes out in declaration order no matter which
    // worker finishes first
    auto tasks = std::vector<ShaderTask>(request.shaders.size());
    {
      auto scope = ParallelScope(impl->job_manager);
      for (auto shader_index = 0_sz; shader_index < request.shaders.size(); shader_index++) {
        scope.dispatch([&shader_session, &request, &task = tasks[shader_index], shader_index] {
          task.result = shader_session.compile_shader(request.shaders[shader_index], task.diag);
        });
      }
    }

    for (auto [shader_index, task] : std::views::enumerate(tasks)) {
      for (auto& error : task.diag.errors) {
        push_error(std::move(error));
      }
      for (auto& message : task.diag.messages) {
        push_message(std::move(message));
      }

      if (!task.result.has_value()) {
        success = false;
        continue;
      }

      const auto& shader = request.shaders[static_cast<usize>(shader_index)];
      auto pipeline = ShaderPipelineData{
        .module_name = shader.module_name,
      };
      pipeline.entry_points.reserve(task.result->size());
      for (auto& ep : task.result.value()) {
        pipeline.entry_points.push_back(std::move(ep));
      }

      pipeline.required_features = shader.required_features;
      impl->asset_file.add_entry(std::move(pipeline));
    }
  }

  return success;
}

auto Session::write_to_file(const std::filesystem::path& output_path) -> bool {
  return impl->asset_file.pack(output_path);
}

auto Session::reflect_layouts(const ShaderSessionInfo& session_info, const std::filesystem::path& path)
  -> std::expected<std::vector<ReflectedType>, std::string> {
  ZoneScoped;

  // same session options as the shader build, reflection offsets only mean anything under the
  // scalar layout the real pipelines are compiled with
  auto slang_session = create_shader_session(impl->slang_global_session, session_info);
  if (!slang_session) {
    return std::unexpected(fmt::format("Failed to create shader session '{}'.", session_info.name));
  }

  const auto source = File::to_string(path);
  if (source.empty()) {
    return std::unexpected(fmt::format("'{}' is empty or unreadable.", path.string()));
  }

  const auto module_name = path.stem().string();
  const auto path_str = path.string();
  auto diagnostics = Slang::ComPtr<slang::IBlob>();
  auto* module = slang_session->loadModuleFromSourceString(
    module_name.c_str(),
    path_str.c_str(),
    source.c_str(),
    diagnostics.writeRef()
  );
  if (!module) {
    auto message = diagnostics ? std::string(
                                   static_cast<const c8*>(diagnostics->getBufferPointer()),
                                   diagnostics->getBufferSize()
                                 )
                               : std::string("no diagnostics");
    return std::unexpected(fmt::format("Failed to load '{}': {}", path_str, message));
  }

  auto* program_layout = module->getLayout();
  auto* module_decl = module->getModuleReflection();
  if (!program_layout || !module_decl) {
    return std::unexpected(fmt::format("'{}' has no reflection data.", path_str));
  }

  auto types = std::vector<ReflectedType>();
  for (u32 child_index = 0; child_index < module_decl->getChildrenCount(); child_index++) {
    auto* decl = module_decl->getChild(child_index);
    const auto kind = decl->getKind();
    if (kind != slang::DeclReflection::Kind::Struct && kind != slang::DeclReflection::Kind::Enum) {
      continue;
    }

    auto* type_layout = program_layout->getTypeLayout(decl->getType(), slang::LayoutRules::DefaultStructuredBuffer);
    if (!type_layout) {
      return std::unexpected(fmt::format("No layout for '{}'.", decl->getName()));
    }

    auto& type = types.emplace_back(ReflectedType{.name = decl->getName(), .size = type_layout->getSize()});
    if (kind != slang::DeclReflection::Kind::Struct) {
      continue;
    }

    for (u32 field_index = 0; field_index < type_layout->getFieldCount(); field_index++) {
      auto* field = type_layout->getFieldByIndex(field_index);
      type.fields.push_back({
        .name = field->getName(),
        .offset = field->getOffset(),
        .size = field->getTypeLayout()->getSize(),
      });
    }
  }

  return types;
}

auto Session::process(const TextureCompileRequest& request) -> option<TextureData> {
  return compile_texture(*this, request);
}

auto Session::process(const ModelCompileRequest& request) -> option<ModelCompileResult> {
  return compile_model(*this, request);
}

auto Session::process(const ProceduralMeshRequest& request) -> option<ModelData> {
  return compile_procedural_mesh(*this, request);
}

} // namespace ox::rc
