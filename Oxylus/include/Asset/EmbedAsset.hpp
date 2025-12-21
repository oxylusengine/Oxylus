#pragma once

#include <string>
#include <vuk/runtime/vk/VkRuntime.hpp>

namespace ox {
class EmbedAsset {
public:
  static auto embed_texture(
    const std::string& tex_file_path, const std::string& out_path, const std::string& array_name
  ) -> void;
  static auto embed_shader(vuk::Runtime& runtime, const std::string& shader_path, const std::string& out_path, const std::string& entry_point)
    -> void;
  static auto embed_binary(const std::string& binary_path, const std::string& out_path, const std::string& array_name) -> void;
};
} // namespace ox
