#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <meshoptimizer.h>
#include <queue>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};

namespace ox {
auto get_default_gltf_extensions() -> fastgltf::Extensions {
  auto extensions = fastgltf::Extensions::None;
  extensions |= fastgltf::Extensions::KHR_mesh_quantization;
  extensions |= fastgltf::Extensions::KHR_texture_transform;
  extensions |= fastgltf::Extensions::KHR_texture_basisu;
  extensions |= fastgltf::Extensions::KHR_lights_punctual;
  extensions |= fastgltf::Extensions::KHR_materials_specular;
  extensions |= fastgltf::Extensions::KHR_materials_ior;
  extensions |= fastgltf::Extensions::KHR_materials_iridescence;
  extensions |= fastgltf::Extensions::KHR_materials_volume;
  extensions |= fastgltf::Extensions::KHR_materials_transmission;
  extensions |= fastgltf::Extensions::KHR_materials_clearcoat;
  extensions |= fastgltf::Extensions::KHR_materials_emissive_strength;
  extensions |= fastgltf::Extensions::KHR_materials_sheen;
  extensions |= fastgltf::Extensions::KHR_materials_unlit;
  extensions |= fastgltf::Extensions::KHR_materials_anisotropy;
  extensions |= fastgltf::Extensions::EXT_meshopt_compression;
  extensions |= fastgltf::Extensions::EXT_texture_webp;
  extensions |= fastgltf::Extensions::MSFT_texture_dds;

  return extensions;
}

auto get_default_gltf_options() -> fastgltf::Options {
  auto options = fastgltf::Options::None;
  options |= fastgltf::Options::LoadExternalBuffers;
  // options |= fastgltf::Options::DontRequireValidAssetMember;

  return options;
}

auto gltf_mime_type_to_asset_file_type(fastgltf::MimeType mime) -> AssetFileType {
  switch (mime) {
    case fastgltf::MimeType::JPEG: return AssetFileType::JPEG;
    case fastgltf::MimeType::PNG : return AssetFileType::PNG;
    case fastgltf::MimeType::KTX2: return AssetFileType::KTX2;
    case fastgltf::MimeType::DDS : return AssetFileType::DDS;
    default                      : return AssetFileType::None;
  }
}

auto gltf_mime_type_to_texture_mime_type(fastgltf::MimeType mime) -> TextureSourceType {
  switch (mime) {
    case fastgltf::MimeType::KTX2: return TextureSourceType::KTX;
    case fastgltf::MimeType::DDS : return TextureSourceType::DDS;
    case fastgltf::MimeType::JPEG:
    case fastgltf::MimeType::PNG :
    default                      : return TextureSourceType::Generic;
  }
}

auto gltf_sampler_to_sampler(const fastgltf::Sampler& gltf_sampler) -> vuk::SamplerCreateInfo {
  auto get_address_mode = [](fastgltf::Wrap v) -> vuk::SamplerAddressMode {
    switch (v) {
      case fastgltf::Wrap::ClampToEdge   : return vuk::SamplerAddressMode::eClampToEdge;
      case fastgltf::Wrap::MirroredRepeat: return vuk::SamplerAddressMode::eMirroredRepeat;
      case fastgltf::Wrap::Repeat        : return vuk::SamplerAddressMode::eRepeat;
    }
  };

  auto get_filter_mode = [](fastgltf::Filter v) -> vuk::Filter {
    switch (v) {
      case fastgltf::Filter::Nearest:
      case fastgltf::Filter::NearestMipMapNearest:
      case fastgltf::Filter::NearestMipMapLinear : return vuk::Filter::eNearest;
      case fastgltf::Filter::Linear              :
      case fastgltf::Filter::LinearMipMapNearest :
      case fastgltf::Filter::LinearMipMapLinear  : return vuk::Filter::eLinear;
    }
  };

  auto get_mip_filter_mode = [](fastgltf::Filter v) -> vuk::SamplerMipmapMode {
    switch (v) {
      case fastgltf::Filter::Nearest:
      case fastgltf::Filter::NearestMipMapNearest:
      case fastgltf::Filter::NearestMipMapLinear : return vuk::SamplerMipmapMode::eNearest;
      case fastgltf::Filter::Linear              :
      case fastgltf::Filter::LinearMipMapNearest :
      case fastgltf::Filter::LinearMipMapLinear  : return vuk::SamplerMipmapMode::eLinear;
    }
  };

  return vuk::SamplerCreateInfo{
    .magFilter = get_filter_mode(gltf_sampler.magFilter.value_or(fastgltf::Filter::Linear)),
    .minFilter = get_filter_mode(gltf_sampler.minFilter.value_or(fastgltf::Filter::Linear)),
    .mipmapMode = get_mip_filter_mode(gltf_sampler.minFilter.value_or(fastgltf::Filter::Linear)),
    .addressModeU = get_address_mode(gltf_sampler.wrapS),
    .addressModeV = get_address_mode(gltf_sampler.wrapT),
  };
}

// Priority: DDS > KTX2/basisu > WebP > base imageIndex
auto get_effective_image_index(const fastgltf::Texture& texture) -> option<usize> {
  if (texture.ddsImageIndex.has_value())
    return texture.ddsImageIndex.value();
  if (texture.basisuImageIndex.has_value())
    return texture.basisuImageIndex.value();
  if (texture.webpImageIndex.has_value())
    return texture.webpImageIndex.value();
  if (texture.imageIndex.has_value())
    return texture.imageIndex.value();
  return nullopt;
}

auto get_mime_type(const fastgltf::Image& image) -> fastgltf::MimeType {
  return std::visit(
    ox::match{
      [](const fastgltf::sources::BufferView& v) { return v.mimeType; },
      [](const fastgltf::sources::Array& v) { return v.mimeType; },
      [](const fastgltf::sources::URI& v) { return v.mimeType; },
      [](const auto&) { return fastgltf::MimeType::None; },
    },
    image.data
  );
}

auto gltf_alpha_mode_to_alpha_mode(fastgltf::AlphaMode mode) -> AlphaMode {
  switch (mode) {
    case fastgltf::AlphaMode::Opaque: return AlphaMode::Opaque;
    case fastgltf::AlphaMode::Mask  : return AlphaMode::Mask;
    case fastgltf::AlphaMode::Blend : return AlphaMode::Blend;
  }
}

auto gltf_material_to_material(const fastgltf::Material& gltf_material, std::span<UUID> textures) -> Material {
  auto material = Material{};

  // PBR
  const auto& pbr = gltf_material.pbrData;
  material.albedo_color = glm::vec4(
    pbr.baseColorFactor.x(),
    pbr.baseColorFactor.y(),
    pbr.baseColorFactor.z(),
    pbr.baseColorFactor.w()
  );
  material.roughness_factor = pbr.roughnessFactor;
  material.metallic_factor = pbr.metallicFactor;

  // Alpha
  material.alpha_mode = gltf_alpha_mode_to_alpha_mode(gltf_material.alphaMode);
  material.alpha_cutoff = gltf_material.alphaCutoff;

  // Emission
  material.emissive_color = glm::vec3(
    gltf_material.emissiveFactor.x(),
    gltf_material.emissiveFactor.y(),
    gltf_material.emissiveFactor.z()
  );
  material.emissive_color *= gltf_material.emissiveStrength;

  // Textures
  auto resolve_texture = [&](const fastgltf::TextureInfo& info) -> UUID {
    if (info.textureIndex < textures.size()) {
      return textures[info.textureIndex];
    }
    return UUID{};
  };

  auto resolve_uv_transform = [&](const fastgltf::TextureInfo& info) {
    if (info.transform) {
      material.uv_offset = glm::vec2(info.transform->uvOffset[0], info.transform->uvOffset[1]);
      material.uv_size = glm::vec2(info.transform->uvScale[0], info.transform->uvScale[1]);
    }
  };

  if (pbr.baseColorTexture.has_value()) {
    material.albedo_texture = resolve_texture(pbr.baseColorTexture.value());
    resolve_uv_transform(pbr.baseColorTexture.value());
  }

  if (pbr.metallicRoughnessTexture.has_value()) {
    material.metallic_roughness_texture = resolve_texture(pbr.metallicRoughnessTexture.value());
  }

  if (gltf_material.normalTexture.has_value()) {
    material.normal_texture = resolve_texture(gltf_material.normalTexture.value());
  }

  if (gltf_material.occlusionTexture.has_value()) {
    material.occlusion_texture = resolve_texture(gltf_material.occlusionTexture.value());
  }

  if (gltf_material.emissiveTexture.has_value()) {
    material.emissive_texture = resolve_texture(gltf_material.emissiveTexture.value());
  }

  return material;
}

auto AssetManager::write_gltf_meta(AssetManager& self, const std::filesystem::path& path, JsonWriter& json) -> bool {
  ZoneScoped;

  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
  auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
  if (gltf_type == fastgltf::GltfType::Invalid) {
    OX_LOG_ERROR("GLTF model type is invalid!");
    return false;
  }

  auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_gltf_options());
  if (!gltf_result) {
    OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(gltf_result.error()));
    return false;
  }

  auto gltf_asset = std::move(gltf_result.get());
  json["embedded_textures"].begin_array();
  for (const auto& [gltf_texture, texture_index] : std::views::zip(gltf_asset.textures, std::views::iota(0_u32))) {
    auto image_index = get_effective_image_index(gltf_texture);
    if (!image_index.has_value()) {
      continue;
    }

    auto& image = gltf_asset.images[image_index.value()];

    if (std::get_if<fastgltf::sources::URI>(&image.data) != nullptr) {
      continue;
    }

    json.begin_obj();
    json["uuid"] = UUID::generate_random().str();
    json["texture_index"] = texture_index;
    json.end_obj();
  }
  json.end_array();

  json["materials"].begin_array();
  for (const auto& v : gltf_asset.materials) {
    json << UUID::generate_random().str();
  }
  json.end_array();

  return true;
}

using IndexMap = ankerl::unordered_dense::map<usize, UUID>;

// TASKS

auto extract_embedded_texture_uuids(simdjson::ondemand::document& doc) -> option<IndexMap> {
  ZoneScoped;

  auto result = ankerl::unordered_dense::map<usize, UUID>{};
  for (auto obj_json : doc["embedded_textures"].get_array()) {
    if (obj_json.error()) {
      OX_LOG_ERROR("Bad embedded_textures entry.");
      return nullopt;
    }

    auto uuid_json = obj_json["uuid"].get_string();
    auto tex_index_json = obj_json["texture_index"].get_uint64();

    if (uuid_json.error() || tex_index_json.error()) {
      OX_LOG_ERROR("Corrupt embedded_textures entry.");
      return nullopt;
    }

    auto texture_uuid = UUID::from_string(uuid_json.value_unsafe());
    if (!texture_uuid.has_value()) {
      OX_LOG_ERROR("Corrupt UUID in embedded_textures.");
      return nullopt;
    }

    result.emplace(static_cast<usize>(tex_index_json.value_unsafe()), texture_uuid.value());
  }

  return result;
}

auto import_gltf_textures(
  AssetManager& self,
  const fastgltf::Asset& asset,
  const std::filesystem::path& asset_path,
  const IndexMap& embedded_texture_uuids
) -> std::vector<UUID> {
  ZoneScoped;

  auto result = std::vector<UUID>();

  for (const auto& [gltf_texture, texture_index] : std::views::zip(asset.textures, std::views::iota(0_sz))) {
    auto image_index = get_effective_image_index(gltf_texture);
    if (!image_index.has_value()) {
      result.push_back(UUID(nullptr));
      continue;
    }

    const auto& image = asset.images[image_index.value()];

    auto texture_uuid = UUID(nullptr);
    if (const auto* source = std::get_if<fastgltf::sources::URI>(&image.data)) {
      texture_uuid = self.import_asset(asset_path.parent_path() / source->uri.fspath());
    } else {
      if (auto it = embedded_texture_uuids.find(texture_index); it != embedded_texture_uuids.end()) {
        texture_uuid = it->second;
        self.register_asset(texture_uuid, AssetType::Texture, asset_path);
      }
    }

    result.push_back(texture_uuid);
  }

  return result;
}

auto extract_linear_texture_indices(const fastgltf::Asset& asset) -> ankerl::unordered_dense::set<usize> {
  ZoneScoped;

  auto result = ankerl::unordered_dense::set<usize>{};
  for (const auto& material : asset.materials) {
    if (material.normalTexture.has_value())
      result.insert(material.normalTexture->textureIndex);
    if (material.pbrData.metallicRoughnessTexture.has_value())
      result.insert(material.pbrData.metallicRoughnessTexture->textureIndex);
    if (material.occlusionTexture.has_value())
      result.insert(material.occlusionTexture->textureIndex);

    if (material.clearcoat) {
      if (material.clearcoat->clearcoatRoughnessTexture.has_value())
        result.insert(material.clearcoat->clearcoatRoughnessTexture->textureIndex);
      if (material.clearcoat->clearcoatNormalTexture.has_value())
        result.insert(material.clearcoat->clearcoatNormalTexture->textureIndex);
    }
    if (material.sheen) {
      if (material.sheen->sheenRoughnessTexture.has_value())
        result.insert(material.sheen->sheenRoughnessTexture->textureIndex);
    }
    if (material.specular) {
      if (material.specular->specularTexture.has_value())
        result.insert(material.specular->specularTexture->textureIndex);
    }
    if (material.transmission) {
      if (material.transmission->transmissionTexture.has_value())
        result.insert(material.transmission->transmissionTexture->textureIndex);
    }
    if (material.anisotropy) {
      if (material.anisotropy->anisotropyTexture.has_value())
        result.insert(material.anisotropy->anisotropyTexture->textureIndex);
    }
  }

  return result;
}

auto load_gltf_texture(
  AssetManager& self,
  const fastgltf::Asset& asset,
  const std::filesystem::path& asset_path,
  const UUID& texture_uuid,
  const fastgltf::Image& gltf_image,
  const fastgltf::Texture& gltf_texture,
  bool is_srgb
) -> void {
  ZoneScoped;

  auto texture_load_info = TextureLoadInfo{
    .is_srgb = is_srgb,
  };

  std::visit(
    ox::match{
      [](const auto&) {},
      [&](const fastgltf::sources::BufferView& v) {
        // Embedded buffer
        auto& buffer_view = asset.bufferViews[v.bufferViewIndex];
        auto& buffer = asset.buffers[buffer_view.bufferIndex];
        std::visit(
          ox::match{
            [](const auto&) {},
            [&](const fastgltf::sources::Array& array) {
              texture_load_info.source = std::span(
                reinterpret_cast<const u8*>(array.bytes.data() + buffer_view.byteOffset),
                buffer_view.byteLength
              );
            },
          },
          buffer.data
        );
      },
      [&](const fastgltf::sources::Array& v) {
        texture_load_info.source = std::span(reinterpret_cast<const u8*>(v.bytes.data()), v.bytes.size_bytes());
      },
      [&](const fastgltf::sources::URI& uri) {
        // External file, resolved relative to the glTF's own directory.
        texture_load_info.source = asset_path.parent_path() / uri.uri.fspath();
      },
    },
    gltf_image.data
  );

  if (gltf_texture.samplerIndex.has_value()) {
    const auto& sampler = asset.samplers[gltf_texture.samplerIndex.value()];
    texture_load_info.sampler_info = gltf_sampler_to_sampler(sampler);
  }

  self.load_asset(texture_uuid, std::move(texture_load_info), false);
}

auto register_gltf_materials(
  AssetManager& self, simdjson::ondemand::document& doc, const std::filesystem::path& asset_path
) -> option<std::vector<UUID>> {
  ZoneScoped;

  auto result = std::vector<UUID>{};
  auto materials_json = doc["materials"];
  for (auto obj_json : materials_json) {
    if (obj_json.error() || !obj_json.is_string()) {
      OX_LOG_ERROR("Bad `embdedded_materials` field.");
      return nullopt;
    }

    auto material_uuid = UUID::from_string(obj_json.get_string());
    if (!material_uuid.has_value()) {
      OX_LOG_ERROR("A material with corrupt UUID.");
      return nullopt;
    }

    self.register_asset(material_uuid.value(), AssetType::Material, asset_path);
    result.emplace_back(material_uuid.value());
  }

  return result;
}

struct MeshBuildData {
  GPU::Mesh gpu_mesh = {};
  std::array<GPU::MeshLOD, GPU::Mesh::MAX_LODS> lods = {};
  std::vector<u8> blob = {};
  u64 lod_metadata_offset = 0;
  bool has_texture_coords = false;
};

auto blob_append(std::vector<u8>& blob, const void* data, usize size, usize alignment) -> u64 {
  const auto offset = ox::align_up(blob.size(), alignment);
  blob.resize(offset + size);
  if (size != 0) {
    std::memcpy(blob.data() + offset, data, size);
  }

  return offset;
}

template <typename T>
auto blob_append(std::vector<u8>& blob, const std::vector<T>& data, usize alignment) -> u64 {
  return blob_append(blob, data.data(), ox::size_bytes(data), alignment);
}

auto build_gltf_mesh(const fastgltf::Asset& gltf_asset, const fastgltf::Primitive& gltf_primitive)
  -> option<MeshBuildData> {
  ZoneScoped;

  if (!gltf_primitive.indicesAccessor.has_value()) {
    return nullopt;
  }

  auto build = MeshBuildData{};

  auto& index_accessor = gltf_asset.accessors[gltf_primitive.indicesAccessor.value()];
  auto raw_indices = std::vector<u32>(index_accessor.count);
  fastgltf::iterateAccessorWithIndex<u32>(gltf_asset, index_accessor, [&](u32 index, usize i) {
    raw_indices[i] = index;
  });

  auto vertex_count = 0_u32;
  auto vertex_remap = std::vector<u32>();
  auto positions = std::vector<glm::vec3>();
  auto quantized_positions = std::vector<glm::u16vec4>();
  auto quantized_normals = std::vector<u32>();
  auto quantized_texcoords = std::vector<glm::u16vec2>();
  if (auto attrib = gltf_primitive.findAttribute("POSITION"); attrib != gltf_primitive.attributes.end()) {
    auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
    auto raw_positions = std::vector<glm::vec3>(accessor.count);
    vertex_remap.resize(accessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 pos, usize i) {
      raw_positions[i] = pos;
    });

    vertex_count = meshopt_optimizeVertexFetchRemap(
      vertex_remap.data(),
      raw_indices.data(),
      raw_indices.size(),
      raw_positions.size()
    );

    positions.resize(vertex_count);
    meshopt_remapVertexBuffer(
      positions.data(),
      raw_positions.data(),
      raw_positions.size(),
      sizeof(glm::vec3),
      vertex_remap.data()
    );
  }

  auto normals = std::vector<glm::vec3>();
  if (auto attrib = gltf_primitive.findAttribute("NORMAL"); attrib != gltf_primitive.attributes.end()) {
    auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
    auto raw_normals = std::vector<glm::vec3>(accessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, accessor, [&](glm::vec3 normal, usize i) {
      raw_normals[i] = normal;
    });

    normals.resize(vertex_count);
    meshopt_remapVertexBuffer(
      normals.data(),
      raw_normals.data(),
      raw_normals.size(),
      sizeof(glm::vec3),
      vertex_remap.data()
    );
  }

  auto texcoords = std::vector<glm::vec2>();
  if (auto attrib = gltf_primitive.findAttribute("TEXCOORD_0"); attrib != gltf_primitive.attributes.end()) {
    auto& accessor = gltf_asset.accessors[attrib->accessorIndex];
    auto raw_texcoords = std::vector<glm::vec2>(accessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf_asset, accessor, [&](glm::vec2 uv, usize i) {
      raw_texcoords[i] = uv;
    });

    texcoords.resize(vertex_count);
    meshopt_remapVertexBuffer(
      texcoords.data(),
      raw_texcoords.data(),
      raw_texcoords.size(),
      sizeof(glm::vec2),
      vertex_remap.data()
    );
  }

  auto indices = std::vector<u32>(index_accessor.count);
  meshopt_remapIndexBuffer(indices.data(), raw_indices.data(), raw_indices.size(), vertex_remap.data());

  quantized_positions.resize(vertex_count);
  for (const auto& [position, quantized_position] : std::views::zip(positions, quantized_positions)) {
    quantized_position.x = meshopt_quantizeHalf(position.x);
    quantized_position.y = meshopt_quantizeHalf(position.y);
    quantized_position.z = meshopt_quantizeHalf(position.z);
  }

  quantized_normals.resize(vertex_count);
  for (const auto& [normal, quantized_normal] : std::views::zip(normals, quantized_normals)) {
    quantized_normal = ((meshopt_quantizeSnorm(normal.x, 10) + 511) << 20) |
                       ((meshopt_quantizeSnorm(normal.y, 10) + 511) << 10) |
                       (meshopt_quantizeSnorm(normal.z, 10) + 511);
  }

  quantized_texcoords.resize(vertex_count);
  for (const auto& [texcoord, quantized_texcoord] : std::views::zip(texcoords, quantized_texcoords)) {
    quantized_texcoord.x = meshopt_quantizeHalf(texcoord.x);
    quantized_texcoord.y = meshopt_quantizeHalf(texcoord.y);
  }

  auto& gpu_mesh = build.gpu_mesh;
  gpu_mesh.vertex_count = vertex_count;
  gpu_mesh.vertex_positions = blob_append(build.blob, quantized_positions, 8);
  gpu_mesh.vertex_normals = blob_append(build.blob, quantized_normals, 4);
  if (!texcoords.empty()) {
    build.has_texture_coords = true;
    gpu_mesh.texture_coords = blob_append(build.blob, quantized_texcoords, 4);
  }

  auto last_lod_indices = std::vector<u32>();
  for (auto lod_index = 0_sz; lod_index < GPU::Mesh::MAX_LODS; lod_index++) {
    ZoneNamedN(z, "GPU Meshlet Generation", true);

    auto& cur_lod = build.lods[lod_index];
    auto simplified_indices = std::vector<u32>();
    if (lod_index == 0) {
      simplified_indices = std::vector<u32>(indices.begin(), indices.end());
    } else {
      const auto& last_lod = build.lods[lod_index - 1];
      auto lod_index_count = ((last_lod_indices.size() + 5_sz) / 6_sz) * 3_sz;
      simplified_indices.resize(last_lod_indices.size(), 0_u32);
      constexpr auto TARGET_ERROR = std::numeric_limits<f32>::max();
      constexpr f32 NORMAL_WEIGHTS[] = {1.0f, 1.0f, 1.0f};

      auto result_error = 0.0f;
      auto result_index_count = meshopt_simplifyWithAttributes(
        simplified_indices.data(),
        last_lod_indices.data(),
        last_lod_indices.size(),
        reinterpret_cast<const f32*>(positions.data()),
        vertex_count,
        sizeof(glm::vec3),
        reinterpret_cast<const f32*>(normals.data()),
        sizeof(glm::vec3),
        NORMAL_WEIGHTS,
        ox::count_of(NORMAL_WEIGHTS),
        nullptr,
        lod_index_count,
        TARGET_ERROR,
        meshopt_SimplifyLockBorder,
        &result_error
      );

      cur_lod.error = last_lod.error + result_error;
      if (
        result_index_count > (lod_index_count + lod_index_count / 2) || result_error > 0.5 || result_index_count < 6
      ) {
        break;
      }

      simplified_indices.resize(result_index_count);
    }

    if (simplified_indices.size() < 3) {
      break;
    }

    last_lod_indices = simplified_indices;

    meshopt_optimizeVertexCache(
      simplified_indices.data(),
      simplified_indices.data(),
      simplified_indices.size(),
      vertex_count
    );

    auto max_meshlet_count = meshopt_buildMeshletsBound(
      simplified_indices.size(),
      Model::MAX_MESHLET_INDICES,
      Model::MAX_MESHLET_PRIMITIVES
    );
    auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlet_count);
    auto indirect_vertex_indices = std::vector<u32>(max_meshlet_count * Model::MAX_MESHLET_INDICES);
    auto local_triangle_indices = std::vector<u8>(max_meshlet_count * Model::MAX_MESHLET_PRIMITIVES * 3);

    auto meshlet_count = meshopt_buildMeshlets(
      raw_meshlets.data(),
      indirect_vertex_indices.data(),
      local_triangle_indices.data(),
      simplified_indices.data(),
      simplified_indices.size(),
      reinterpret_cast<const f32*>(positions.data()),
      vertex_count,
      sizeof(glm::vec3),
      Model::MAX_MESHLET_INDICES,
      Model::MAX_MESHLET_PRIMITIVES,
      0.0
    );

    if (meshlet_count == 0) {
      break;
    }

    raw_meshlets.resize(meshlet_count);
    auto meshlets = std::vector<GPU::Meshlet>(meshlet_count);
    const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
    indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
    local_triangle_indices.resize(last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) & ~3_u32));

    auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
    auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
    auto gpu_meshlet_bounds = std::vector<GPU::MeshletBounds>(meshlet_count);
    for (const auto& [raw_meshlet, meshlet, bounds] : std::views::zip(raw_meshlets, meshlets, gpu_meshlet_bounds)) {
      auto meshlet_bb_min = glm::vec3(std::numeric_limits<f32>::max());
      auto meshlet_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());
      for (u32 i = 0; i < raw_meshlet.triangle_count * 3; i++) {
        auto local_triangle_index_offset = raw_meshlet.triangle_offset + i;
        OX_ASSERT(local_triangle_index_offset < local_triangle_indices.size());
        auto local_triangle_index = local_triangle_indices[local_triangle_index_offset];
        OX_ASSERT(local_triangle_index < raw_meshlet.vertex_count);
        auto indirect_vertex_index_offset = raw_meshlet.vertex_offset + local_triangle_index;
        OX_ASSERT(indirect_vertex_index_offset < indirect_vertex_indices.size());
        auto indirect_vertex_index = indirect_vertex_indices[indirect_vertex_index_offset];
        OX_ASSERT(indirect_vertex_index < vertex_count);

        const auto& tri_pos = positions[indirect_vertex_index];
        meshlet_bb_min = glm::min(meshlet_bb_min, tri_pos);
        meshlet_bb_max = glm::max(meshlet_bb_max, tri_pos);
      }

      auto meshlet_bounds = meshopt_computeMeshletBounds(
        &indirect_vertex_indices[raw_meshlet.vertex_offset],
        &local_triangle_indices[raw_meshlet.triangle_offset],
        raw_meshlet.triangle_count,
        reinterpret_cast<f32*>(positions.data()),
        vertex_count,
        sizeof(glm::vec3)
      );

      auto meshlet_aabb_center = (meshlet_bb_max + meshlet_bb_min) * 0.5f;
      auto meshlet_aabb_extent = meshlet_bb_max - meshlet_bb_min;

      meshlet.indirect_vertex_index_offset = raw_meshlet.vertex_offset;
      meshlet.local_triangle_index_offset = raw_meshlet.triangle_offset;
      meshlet.vertex_count = raw_meshlet.vertex_count;
      meshlet.triangle_count = raw_meshlet.triangle_count;

      bounds.aabb_center.x = meshopt_quantizeHalf(meshlet_aabb_center.x);
      bounds.aabb_center.y = meshopt_quantizeHalf(meshlet_aabb_center.y);
      bounds.aabb_center.z = meshopt_quantizeHalf(meshlet_aabb_center.z);

      bounds.aabb_extent.x = meshopt_quantizeHalf(meshlet_aabb_extent.x);
      bounds.aabb_extent.y = meshopt_quantizeHalf(meshlet_aabb_extent.y);
      bounds.aabb_extent.z = meshopt_quantizeHalf(meshlet_aabb_extent.z);

      bounds.cone_axis_xy = {meshlet_bounds.cone_axis_s8[0], meshlet_bounds.cone_axis_s8[1]};
      bounds.cone_axis_z = meshlet_bounds.cone_axis_s8[2];
      bounds.cone_cutoff = meshlet_bounds.cone_cutoff_s8;

      mesh_bb_min = glm::min(mesh_bb_min, meshlet_bb_min);
      mesh_bb_max = glm::max(mesh_bb_max, meshlet_bb_max);
    }

    if (lod_index == 0) {
      gpu_mesh.bounds.aabb_center = (mesh_bb_max + mesh_bb_min) * 0.5f;
      gpu_mesh.bounds.aabb_extent = mesh_bb_max - mesh_bb_min;
    }

    cur_lod.indices = blob_append(build.blob, simplified_indices, 8);
    cur_lod.meshlets = blob_append(build.blob, meshlets, 8);
    cur_lod.meshlet_bounds = blob_append(build.blob, gpu_meshlet_bounds, 8);
    cur_lod.local_triangle_indices = blob_append(build.blob, local_triangle_indices, 8);
    cur_lod.indirect_vertex_indices = blob_append(build.blob, indirect_vertex_indices, 4);

    cur_lod.indices_count = simplified_indices.size();
    cur_lod.meshlet_count = meshlet_count;
    cur_lod.meshlet_bounds_count = gpu_meshlet_bounds.size();
    cur_lod.local_triangle_indices_count = local_triangle_indices.size();
    cur_lod.indirect_vertex_indices_count = indirect_vertex_indices.size();

    gpu_mesh.lod_count += 1;
  }

  if (gpu_mesh.lod_count == 0) {
    return nullopt;
  }

  build.lod_metadata_offset = ox::align_up(build.blob.size(), 8);
  build.blob.resize(build.lod_metadata_offset + gpu_mesh.lod_count * sizeof(GPU::MeshLOD));

  return build;
}

auto upload_gltf_mesh(RenderContext& render_context, MeshBuildData& build) -> vuk::Unique<vuk::Buffer> {
  ZoneScoped;

  auto gpu_buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, build.blob.size());
  const auto gpu_mesh_bda = gpu_buffer->device_address;

  build.gpu_mesh.vertex_positions += gpu_mesh_bda;
  build.gpu_mesh.vertex_normals += gpu_mesh_bda;
  if (build.has_texture_coords) {
    build.gpu_mesh.texture_coords += gpu_mesh_bda;
  }
  build.gpu_mesh.lods = gpu_mesh_bda + build.lod_metadata_offset;

  for (auto lod_index = 0_u32; lod_index < build.gpu_mesh.lod_count; lod_index++) {
    auto& lod = build.lods[lod_index];
    lod.indices += gpu_mesh_bda;
    lod.meshlets += gpu_mesh_bda;
    lod.meshlet_bounds += gpu_mesh_bda;
    lod.local_triangle_indices += gpu_mesh_bda;
    lod.indirect_vertex_indices += gpu_mesh_bda;
  }

  std::memcpy(
    build.blob.data() + build.lod_metadata_offset,
    build.lods.data(),
    build.gpu_mesh.lod_count * sizeof(GPU::MeshLOD)
  );

  auto staging_buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eCPUonly, build.blob.size());
  std::memcpy(staging_buffer->mapped_ptr, build.blob.data(), build.blob.size());

  auto gpu_mesh_value = vuk::discard_buf("mesh", *gpu_buffer);
  auto staging_value = vuk::acquire_buf("mesh staging", *staging_buffer, vuk::Access::eNone);
  render_context.wait_on(render_context.upload_staging(std::move(staging_value), std::move(gpu_mesh_value)));

  return gpu_buffer;
}

auto AssetManager::load_model(this AssetManager& self, const std::filesystem::path& path, bool async) -> ModelID {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& job_man = App::get_job_manager();

  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
  auto gltf_type = fastgltf::determineGltfFileType(gltf_buffer.get());
  if (gltf_type == fastgltf::GltfType::Invalid) {
    OX_LOG_ERROR("GLTF model type is invalid!");
    return ModelID::Invalid;
  }

  auto gltf_parser = fastgltf::Parser(get_default_gltf_extensions());
  auto gltf_result = gltf_parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_gltf_options());
  if (!gltf_result) {
    OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(gltf_result.error()));
    return ModelID::Invalid;
  }

  auto gltf_asset_ref = std::make_shared<fastgltf::Asset>(std::move(gltf_result.get()));
  auto& gltf_asset = *gltf_asset_ref;
  if (gltf_asset.scenes.size() != 1) {
    OX_LOG_ERROR("Error loading {}. The GLTF scene can only contain one scene.", path);
    return ModelID::Invalid;
  }

  auto meta_path = std::filesystem::path(path.string() + ".oxasset");
  auto meta_json = self.read_meta_file(meta_path);
  if (!meta_json) {
    return ModelID::Invalid;
  }

  auto embedded_texture_uuids_result = extract_embedded_texture_uuids(*meta_json->doc);
  if (!embedded_texture_uuids_result.has_value()) {
    return ModelID::Invalid;
  }
  auto embedded_texture_uuids = std::move(embedded_texture_uuids_result.value());

  auto linear_texture_indices = extract_linear_texture_indices(gltf_asset);
  auto textures = import_gltf_textures(self, gltf_asset, path, embedded_texture_uuids);

  const auto use_jobs = job_man.get_thread_count() > 1;
  auto dispatch = [&job_man, use_jobs](const Arc<Barrier>& barrier, auto&& work) {
    if (!use_jobs) {
      work();
      return;
    }

    barrier->acquire();
    auto job = Job::create(std::forward<decltype(work)>(work));
    job->signal(barrier);
    job_man.submit(std::move(job));
  };

  OX_ASSERT(gltf_asset.textures.size() == textures.size());
  auto texture_barrier = Barrier::create();
  for (const auto& [gltf_texture, texture_uuid, texture_index] :
       std::views::zip(gltf_asset.textures, textures, std::views::iota(0_sz))) {
    auto image_index = get_effective_image_index(gltf_texture);
    if (!image_index.has_value()) {
      continue;
    }

    auto is_srgb = !linear_texture_indices.contains(texture_index);

    dispatch(
      texture_barrier,
      [&asset_man = self,
       gltf_asset_ref,
       path,
       texture_uuid,
       texture_index,
       gltf_image_index = *image_index,
       is_srgb]() {
        load_gltf_texture(
          asset_man,
          *gltf_asset_ref,
          path,
          texture_uuid,
          gltf_asset_ref->images[gltf_image_index],
          gltf_asset_ref->textures[texture_index],
          is_srgb
        );
      }
    );
  }

  texture_barrier->wait();

  auto materials_result = register_gltf_materials(self, *meta_json->doc, path);
  if (!materials_result.has_value()) {
    return ModelID::Invalid;
  }
  auto materials = std::move(materials_result.value());

  for (const auto& [material_uuid, gltf_material] : std::views::zip(materials, gltf_asset.materials)) {
    self.load_asset(material_uuid, gltf_material_to_material(gltf_material, textures), false);
  }

  auto lights = std::vector<Model::Light>();
  for (const auto& gltf_light : gltf_asset.lights) {
    lights.push_back({
      .name = std::string(gltf_light.name),
      .type = static_cast<Model::LightType>(gltf_light.type),
      .color = glm::make_vec3(gltf_light.color.data()),
      .intensity = gltf_light.intensity,
      .range = gltf_light.range ? option<f32>(*gltf_light.range) : nullopt,
      .inner_cone_angle = gltf_light.innerConeAngle ? option<f32>(*gltf_light.innerConeAngle) : nullopt,
      .outer_cone_angle = gltf_light.outerConeAngle ? option<f32>(*gltf_light.outerConeAngle) : nullopt,
    });
  }

  auto model = Model{};
  model.textures = std::move(textures);
  model.materials = std::move(materials);
  model.lights = std::move(lights);

  auto& gltf_default_scene = gltf_asset.scenes[gltf_asset.defaultScene.value_or(0_sz)];
  struct ProcessingNode {
    usize gltf_node_index = 0;
    usize parent_mesh_group_index = 0;
  };
  auto processing_gltf_nodes = std::queue<ProcessingNode>();

  auto& root_mesh_group = model.mesh_groups.emplace_back();
  root_mesh_group.name = gltf_default_scene.name;
  for (auto node_index : gltf_default_scene.nodeIndices) {
    processing_gltf_nodes.push({node_index, 0});
  }

  struct PendingMesh {
    usize gltf_mesh_index = 0;
    usize gltf_primitive_index = 0;
  };
  auto pending_meshes = std::vector<PendingMesh>();

  while (!processing_gltf_nodes.empty()) {
    auto [gltf_node_index, parent_mesh_group_index] = processing_gltf_nodes.front();
    const auto& node = gltf_asset.nodes[gltf_node_index];
    auto& parent_mesh_group = model.mesh_groups[parent_mesh_group_index];
    processing_gltf_nodes.pop();

    auto mesh_group_index = model.mesh_groups.size();
    parent_mesh_group.child_indices.push_back(mesh_group_index);

    auto& mesh_group = model.mesh_groups.emplace_back();
    mesh_group.name = node.name;

    for (auto child_node_index : node.children) {
      processing_gltf_nodes.push({child_node_index, mesh_group_index});
    }

    auto translation = glm::vec3{};
    auto rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
    auto scale = glm::vec3{1.f, 1.f, 1.f};
    if (auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
      translation = glm::make_vec3(trs->translation.data());
      rotation = glm::quat::wxyz(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
      scale = glm::make_vec3(trs->scale.data());
    } else if (auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
      auto transform_mat = glm::make_mat4x4(mat->data());
      auto skew = glm::vec3{};
      auto perspective = glm::vec4{};
      glm::decompose(transform_mat, scale, rotation, translation, skew, perspective);
    }

    mesh_group.translation = translation;
    mesh_group.rotation = rotation;
    mesh_group.scale = scale;

    if (node.lightIndex.has_value()) {
      mesh_group.light_indices.push_back(node.lightIndex.value());
    }

    if (!node.meshIndex.has_value()) {
      continue;
    }

    const auto gltf_mesh_index = node.meshIndex.value();
    const auto& gltf_mesh = gltf_asset.meshes[gltf_mesh_index];
    for (const auto& [gltf_primitive, gltf_primitive_index] :
         std::views::zip(gltf_mesh.primitives, std::views::iota(0_sz))) {
      if (!gltf_primitive.indicesAccessor.has_value()) {
        continue;
      }

      auto mesh_index = model.gpu_meshes.size();
      mesh_group.mesh_indices.push_back(mesh_index);

      auto mesh_material_index = option<u32>(nullopt);
      if (gltf_primitive.materialIndex.has_value()) {
        mesh_material_index = gltf_primitive.materialIndex.value();
      }

      model.material_indices.push_back(mesh_material_index);
      model.gpu_meshes.emplace_back();
      model.lod0_meshlet_counts.push_back(0_u32);
      model.gpu_mesh_buffers.emplace_back();
      pending_meshes.push_back({gltf_mesh_index, gltf_primitive_index});
    }
  }

  model.mesh_ready = std::vector<std::atomic_flag>(pending_meshes.size());
  model.pending_meshes = static_cast<u32>(pending_meshes.size());

  auto model_id = ModelID::Invalid;
  {
    auto write_lock = std::unique_lock(self.models_mutex);
    model_id = self.model_map.create_slot(std::move(model));
  }

  auto& render_context = App::get_rendercontext();
  auto mesh_barrier = Barrier::create();
  for (const auto& [pending_mesh, mesh_index] : std::views::zip(pending_meshes, std::views::iota(0_sz))) {
    dispatch(mesh_barrier, [&asset_man = self, model_id, gltf_asset_ref, &render_context, pending_mesh, mesh_index]() {
      ZoneScopedN("GLTF Mesh Build");

      const auto& gltf_primitive = gltf_asset_ref->meshes[pending_mesh.gltf_mesh_index]
                                     .primitives[pending_mesh.gltf_primitive_index];
      auto build = build_gltf_mesh(*gltf_asset_ref, gltf_primitive);
      auto mesh_buffer = vuk::Unique<vuk::Buffer>();
      if (build) {
        mesh_buffer = upload_gltf_mesh(render_context, *build);
      }

      auto loaded_model = asset_man.get_model(model_id);
      if (!loaded_model) {
        return;
      }

      if (build) {
        loaded_model->gpu_mesh_buffers[mesh_index] = std::move(mesh_buffer);
        loaded_model->gpu_meshes[mesh_index] = build->gpu_mesh;
        loaded_model->lod0_meshlet_counts[mesh_index] = build->lods[0].meshlet_count;

        loaded_model->mesh_ready[mesh_index].test_and_set(std::memory_order_release);
      }

      auto pending = std::atomic_ref(loaded_model->pending_meshes);
      const auto was_last = pending.fetch_sub(1, std::memory_order_acq_rel) == 1;
      loaded_model.reset();

      if (was_last) {
        asset_man.notify_model_loaded();
      }
    });
  }

  if (!async) {
    mesh_barrier->wait();
  }

  return model_id;
}

auto AssetManager::unload_model(this AssetManager& self, ReadGuard<Asset> asset) -> bool {
  ZoneScoped;

  const auto model_id = asset->model_id;

  self.wait_until_model_loaded(model_id);

  auto write_lock = std::unique_lock(self.models_mutex);
  if (auto* model = self.model_map.slot(model_id)) {
    *model = Model{};
  }
  self.model_map.destroy_slot(model_id);
  asset->model_id = ModelID::Invalid;

  return true;
}

} // namespace ox
