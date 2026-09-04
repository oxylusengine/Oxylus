#pragma once

#include <expected>
#include <filesystem>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Asset/ShaderFeature.hpp"
#include "Core/Handle.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"

#if OX_PLATFORM_WINDOWS
  #ifdef OXRC_EXPORTS
    #if defined(OX_COMPILER_MSVC) || defined(OX_COMPILER_CLANGCL)
      #define OXRC_API __declspec(dllexport)
    #else
      #define OXRC_API __attribute__((dllexport))
    #endif
  #else
    #if defined(OX_COMPILER_MSVC) || defined(OX_COMPILER_CLANGCL)
      #define OXRC_API __declspec(dllimport)
    #else
      #define OXRC_API __attribute__((dllimport))
    #endif
  #endif
#else
  #ifdef OXRC_EXPORTS
    #define OXRC_API __attribute__((visibility("default")))
  #else
    #define OXRC_API
  #endif
#endif

namespace ox::rc {
struct ShaderSessionInfo {
  std::string name = {};
  std::filesystem::path root_directory = {};
  std::vector<std::filesystem::path> include_directories = {};
  i32 optimization_level = 3;
  std::vector<std::pair<std::string, std::string>> definitions = {};
};

struct ShaderCompileInfo {
  std::filesystem::path path = {};
  std::string module_name = {};
  std::vector<std::string> entry_points = {};
  ShaderFeatureFlag required_features = ShaderFeatureFlag::None;
};

struct ShaderCompileRequest {
  ShaderSessionInfo session_info = {};
  std::vector<ShaderCompileInfo> shaders = {};
};

struct TextureCompileRequest {
  std::filesystem::path path = {};
  // takes priority over `path`, for images embedded in a glTF buffer
  std::vector<u8> source_bytes = {};
  std::string name = {};
  // unset honours the colour space the source declares (a KTX2's transfer function, a DDS's DXGI
  // format); a value overrides it, which is what a glTF does since the material slot knows better
  option<bool> srgb = nullopt;
};

struct ModelCompileRequest {
  std::filesystem::path path = {};
  std::string name = {};
};

struct ModelVertex {
  f32 position[3] = {};
  f32 normal[3] = {};
  f32 uv[2] = {};
};

struct ProceduralMeshRequest {
  std::string name = {};
  std::vector<ModelVertex> vertices = {};
  std::vector<u32> indices = {};
};

struct CompiledTexture {
  enum class Kind : u32 {
    None = 0, // the glTF texture has no usable image source
    External, // a sibling file the caller imports as an asset of its own
    Compiled, // decoded here; `data` is ready to pack
  };

  Kind kind = Kind::None;
  std::filesystem::path external_path = {}; // Kind::External, relative to the model file
  TextureData data = {};                    // Kind::Compiled
};

struct ModelCompileResult {
  ModelData model = {};
  std::vector<CompiledTexture> textures = {};
};

struct SessionDiagnostics {
  std::vector<std::string> errors = {};
  std::vector<std::string> messages = {};
};

struct SessionCreateInfo {
  // 0 derives a count from the hardware concurrency
  u32 thread_count = 0;
};

struct OXRC_API Session : Handle<Session> {
  static auto create(const SessionCreateInfo& info = {}) -> option<Session>;
  auto destroy() -> void;

  auto add_request(const ShaderCompileRequest& request) -> void;
  auto compile() -> bool;
  auto write_to_file(const std::filesystem::path& output_path) -> bool;

  auto process(const TextureCompileRequest& request) -> option<TextureData>;
  auto process(const ModelCompileRequest& request) -> option<ModelCompileResult>;
  auto process(const ProceduralMeshRequest& request) -> option<ModelData>;

  auto push_error(std::string msg) -> void;
  auto push_message(std::string msg) -> void;
  // snapshots, because the compilers push from worker threads
  auto get_errors() const -> std::vector<std::string>;
  auto get_messages() const -> std::vector<std::string>;
  // moves both out and clears them, so concurrent callers each report a disjoint slice
  auto take_diagnostics() -> SessionDiagnostics;
};

// msvc rejects an out-of-line definition of an explicit-object member of a dllexport class
// (C2340), so these stay inline in the class
struct OXRC_API ResourceCompiler final : Session {
  constexpr static auto MODULE_NAME = "ResourceCompiler";

  auto init(this ResourceCompiler& self) -> std::expected<void, std::string> {
    auto session = Session::create();
    if (!session.has_value()) {
      return std::unexpected("Failed to create the resource compiler session.");
    }

    static_cast<Session&>(self) = session.value();

    return {};
  }

  auto deinit(this ResourceCompiler& self) -> std::expected<void, std::string> {
    if (self) {
      self.destroy();
    }

    return {};
  }
};

} // namespace ox::rc
