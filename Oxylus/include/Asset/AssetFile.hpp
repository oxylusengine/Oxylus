#pragma once

#include <filesystem>
#include <vuk/Types.hpp>
#include <vuk/runtime/vk/VkTypes.hpp>
#include <zpp_bits.h>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
enum class AssetType : u32 {
  None = 0,
  Shader,
  Model,
  Texture,
  Material,
  Font,
  Scene,
  Audio,
  Script,
};

// List of file extensions supported by Engine.
enum class AssetFileType : u32 {
  None = 0,
  Binary,
  Meta,
  GLB,
  GLTF,
  PNG,
  JPEG,
  DDS,
  JSON,
  KTX2,
  LUA,
};

struct NoneAsset {
  using serialize_id = zpp::bits::serialization_id<AssetType::None>;
};

struct ShaderEntryPointData {
  std::string name = {};
  u32 shader_stage = {};
  std::vector<u32> spirv = {};
};

struct ShaderPipelineData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Shader>;

  std::string module_name = "";
  std::vector<ShaderEntryPointData> entry_points = {};
  bool bindless = false;
};

struct AssetFileEntry {
  AssetType type = AssetType::None;
  std::variant<NoneAsset, ShaderPipelineData> data;

  constexpr static auto serialize(auto& archive, auto& self)
    requires(std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::in)
  {
    if (auto err = archive(self.type); zpp::bits::failure(err))
      return err;
    return archive(zpp::bits::known_id(self.type, self.data));
  }

  constexpr static auto serialize(auto& archive, auto& self)
    requires(std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::out)
  {
    auto _ = archive(self.type);
    return std::visit([&](auto& v) { return archive(v); }, self.data);
  }
};

enum class AssetFileFlags : u32 {
  None = 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct AssetFileHeader {
  static constexpr auto SIGNATURE = 0x4352584F_u32;
  static constexpr auto VERSION = 1_u16;

  u32 magic = SIGNATURE; // "OXRC"
  u16 version = VERSION;
  AssetFileFlags flags = AssetFileFlags::None;
};

struct AssetFile {
  AssetFileFlags flags = AssetFileFlags::None;
  std::vector<AssetFileEntry> entries = {};

  static auto unpack(const std::filesystem::path& path) -> option<AssetFile>;
  auto pack(this AssetFile& self, const std::filesystem::path& path) -> bool;
  auto add_entry(this AssetFile& self, ShaderPipelineData&& entry) -> void;
};
} // namespace ox
