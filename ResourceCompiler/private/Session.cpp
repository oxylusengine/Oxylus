#include "Session.hpp"

#include <zpp_bits.h>

#include "OS/File.hpp"
#include "ShaderSession.hpp"
#include "Utils/Log.hpp"

namespace ox::rc {
auto create_shader_session(slang::IGlobalSession* global_session, const ShaderSessionInfo& info)
  -> Slang::ComPtr<slang::ISession> {
  slang::CompilerOptionEntry entries[] = {
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
  if (SLANG_FAILED(global_session->createSession(session_desc, session.writeRef()))) {
    return nullptr;
  }

  return session;
}

auto Session::create() -> option<Session> {
  auto* self = new Session::Impl;
  auto write_lock = std::unique_lock(self->session_mutex);
  if (SLANG_FAILED(slang::createGlobalSession(self->slang_global_session.writeRef()))) {
    delete self;
    return nullopt;
  }

  return Session(self);
}

auto Session::destroy() -> void {
  delete impl;
  impl = nullptr;
}

auto Session::add_request(const ShaderCompileRequest& request) -> void {
  impl->shader_requests.emplace_back(request);
}

auto Session::push_error(std::string msg) -> void {
  auto lock = std::unique_lock(impl->messages_mutex);
  impl->errors.push_back(std::move(msg));
}

auto Session::push_message(std::string msg) -> void {
  auto lock = std::unique_lock(impl->messages_mutex);
  impl->messages.push_back(std::move(msg));
}

auto Session::get_errors() const -> const std::vector<std::string>& { return impl->errors; }

auto Session::get_messages() const -> const std::vector<std::string>& { return impl->messages; }

auto Session::compile() -> bool {
  bool success = true;

  for (const auto& request : impl->shader_requests) {
    auto slang_session = create_shader_session(impl->slang_global_session, request.session_info);
    if (!slang_session) {
      push_error(fmt::format("Failed to create shader session '{}'.", request.session_info.name));
      success = false;
      continue;
    }

    auto shader_session = ShaderSession{
      .rc_session = *this,
      .name = request.session_info.name,
      .slang_session = slang_session.get(),
      .root_directory = request.session_info.root_directory,
    };

    for (const auto& shader : request.shaders) {
      auto entry_points = shader_session.compile_shader(shader);
      if (!entry_points.has_value()) {
        success = false;
        continue;
      }

      auto pipeline = ShaderPipelineData{
        .module_name = shader.module_name,
      };
      pipeline.entry_points.reserve(entry_points->size());
      for (auto& ep : entry_points.value()) {
        pipeline.entry_points.push_back(std::move(ep));
      }
      impl->compiled_pipelines.push_back(std::move(pipeline));
    }
  }

  return success;
}

auto Session::write_to_file(const std::filesystem::path& output_path) -> bool {
  auto header = AssetFileHeader{
    .type = AssetType::Shader,
    .entry_count = static_cast<u32>(impl->compiled_pipelines.size()),
  };

  auto [data, out] = zpp::bits::data_out();
  if (failure(out(header, impl->compiled_pipelines))) {
    push_error(fmt::format("Failed to serialize shader asset to '{}'.", output_path));
    return false;
  }

  auto file = File(output_path, FileAccess::Write);
  if (!file) {
    push_error(fmt::format("Failed to open '{}' for writing.", output_path));
    return false;
  }

  file.write(data);
  return true;
}

auto read_shader_asset(
  const std::filesystem::path& path, AssetFileHeader& out_header, std::vector<ShaderPipelineData>& out_pipelines
) -> bool {
  auto file_data = File::to_bytes(path);
  if (file_data.empty()) {
    OX_LOG_ERROR("Failed to read shader asset from '{}'.", path);
    return false;
  }

  zpp::bits::in in(file_data);
  if (failure(in(out_header, out_pipelines))) {
    OX_LOG_ERROR("Failed to deserialize shader asset from '{}'.", path);
    return false;
  }

  if (out_header.magic[0] != 'O' || out_header.magic[1] != 'X') {
    OX_LOG_ERROR("Invalid shader asset magic in '{}'.", path);
    return false;
  }

  if (out_header.type != AssetType::Shader) {
    OX_LOG_ERROR("Asset '{}' is not a shader asset.", path);
    return false;
  }

  return true;
}

} // namespace ox::rc
