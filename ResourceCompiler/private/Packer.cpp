#include "Packer.hpp"

namespace ox::rc {
auto Packer::add_request(const ShaderCompileRequest& request) -> void { impl->shader_requests.emplace_back(request); }

auto Packer::add_request(const TextureCompileRequest& request) -> void { impl->texture_requests.emplace_back(request); }

auto Packer::add_request(const ModelCompileRequest& request) -> void { impl->model_requests.emplace_back(request); }

auto Packer::pack() -> bool {
  bool success = true;

  for (const auto& request : impl->shader_requests) {
    auto shaders = impl->session.process(request);
    if (!shaders.has_value()) {
      impl->session.push_error(fmt::format("Failed to compile pipeline '{}'.", request.session_info.root_directory));
      success = false;
      continue;
    }

    for (auto&& shader : shaders.value())
      impl->asset_file.add_entry(std::move(shader));
  }

  for (const auto& request : impl->texture_requests) {
    auto texture_data = impl->session.process(request);
    if (!texture_data.has_value()) {
      impl->session.push_error(fmt::format("Failed to compile texture '{}'.", request.path.string()));
      success = false;
      continue;
    }

    impl->asset_file.add_entry(std::move(texture_data.value()));
  }

  for (const auto& request : impl->model_requests) {
    auto model_data = impl->session.process(request);
    if (!model_data.has_value()) {
      impl->session.push_error(fmt::format("Failed to compile model '{}'.", request.path.string()));
      success = false;
      continue;
    }

    impl->asset_file.add_entry(std::move(model_data.value()));
  }

  return success;
}

auto Packer::write_to_file(const std::filesystem::path& output_path) -> bool {
  return impl->asset_file.pack(output_path);
}

} // namespace ox::rc
