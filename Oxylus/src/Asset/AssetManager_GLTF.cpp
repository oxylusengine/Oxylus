#include <algorithm>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <meshoptimizer.h>
#include <numeric>
#include <queue>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/AccelerationStructure.hpp"
#include "Render/UploadBatch.hpp"

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};
template <>
struct fastgltf::ElementTraits<glm::u16vec4>
    : fastgltf::ElementTraitsBase<glm::u16vec4, AccessorType::Vec4, std::uint16_t> {};

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

    return vuk::SamplerAddressMode::eRepeat;
  };

  auto get_filter_mode = [](fastgltf::Filter v) -> vuk::Filter {
    switch (v) {
      case fastgltf::Filter::Nearest             :
      case fastgltf::Filter::NearestMipMapNearest:
      case fastgltf::Filter::NearestMipMapLinear : return vuk::Filter::eNearest;
      case fastgltf::Filter::Linear              :
      case fastgltf::Filter::LinearMipMapNearest :
      case fastgltf::Filter::LinearMipMapLinear  : return vuk::Filter::eLinear;
    }

    return vuk::Filter::eLinear;
  };

  auto get_mip_filter_mode = [](fastgltf::Filter v) -> vuk::SamplerMipmapMode {
    switch (v) {
      case fastgltf::Filter::Nearest             :
      case fastgltf::Filter::NearestMipMapNearest:
      case fastgltf::Filter::NearestMipMapLinear : return vuk::SamplerMipmapMode::eNearest;
      case fastgltf::Filter::Linear              :
      case fastgltf::Filter::LinearMipMapNearest :
      case fastgltf::Filter::LinearMipMapLinear  : return vuk::SamplerMipmapMode::eLinear;
    }

    return vuk::SamplerMipmapMode::eLinear;
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

  return AlphaMode::Opaque;
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
  for (auto i = 0_sz; i < gltf_asset.materials.size(); i++) {
    json << UUID::generate_random().str();
  }
  json.end_array();

  json["skeletons"].begin_array();
  for (auto i = 0_sz; i < gltf_asset.skins.size(); i++) {
    json << UUID::generate_random().str();
  }
  json.end_array();

  json["animations"].begin_array();
  for (const auto& [gltf_animation, animation_index] :
       std::views::zip(gltf_asset.animations, std::views::iota(0_u32))) {
    json.begin_obj();
    json["uuid"] = UUID::generate_random().str();
    json["name"] = std::string(gltf_animation.name);
    json["animation_index"] = animation_index;
    json.end_obj();
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
  bool is_srgb,
  UploadBatch* batch
) -> void {
  ZoneScoped;

  auto texture_load_info = TextureLoadInfo{
    .is_srgb = is_srgb,
    .batch = batch,
  };

  std::visit(
    ox::match{
      [](const auto&) { OX_LOG_ERROR("Unsupported glTF image data source; the texture will not be loaded."); },
      [&](const fastgltf::sources::BufferView& v) {
        // Embedded buffer
        auto& buffer_view = asset.bufferViews[v.bufferViewIndex];
        auto& buffer = asset.buffers[buffer_view.bufferIndex];
        std::visit(
          ox::match{
            [](const auto&) { OX_LOG_ERROR("Unsupported glTF buffer data source; the texture will not be loaded."); },
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

struct SkinBuildData {
  Skeleton skeleton = {};
  // glTF skin joint slot -> bone index
  std::vector<u32> joint_to_bone = {};
  // bone index -> glTF node index
  std::vector<usize> bone_to_node = {};
  // chain from the scene root down to the parent of the skin's root joints, because glTF ignores
  // the skinned mesh node's own transform yet the joints still live under whatever ancestors the
  // exporter emitted, and those have to be folded back in somewhere
  BoneTransform root_prefix = {};
};

auto gltf_node_local_transform(const fastgltf::Node& node) -> BoneTransform {
  if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
    const auto scale = glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]);
    return BoneTransform::from_trs(
      glm::vec3(trs->translation[0], trs->translation[1], trs->translation[2]),
      glm::quat::wxyz(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]),
      (scale.x + scale.y + scale.z) / 3.f
    );
  }

  const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(&node.transform);
  return matrix ? BoneTransform::from_mat4(glm::make_mat4(matrix->data())) : BoneTransform{};
}

auto build_gltf_node_parents(const fastgltf::Asset& gltf_asset) -> std::vector<usize> {
  auto parents = std::vector<usize>(gltf_asset.nodes.size(), ~0_sz);
  for (const auto& [node, node_index] : std::views::zip(gltf_asset.nodes, std::views::iota(0_sz))) {
    for (const auto child : node.children) {
      parents[child] = node_index;
    }
  }

  return parents;
}

auto build_gltf_skeleton(const fastgltf::Asset& gltf_asset, const fastgltf::Skin& gltf_skin) -> option<SkinBuildData> {
  ZoneScoped;

  const auto joint_count = gltf_skin.joints.size();
  if (joint_count == 0) {
    return nullopt;
  }

  const auto node_parents = build_gltf_node_parents(gltf_asset);

  auto node_to_joint = ankerl::unordered_dense::map<usize, u32>();
  node_to_joint.reserve(joint_count);
  for (const auto& [node_index, joint_slot] : std::views::zip(gltf_skin.joints, std::views::iota(0_u32))) {
    node_to_joint.emplace(static_cast<usize>(node_index), joint_slot);
  }

  // depth-first from every joint whose parent is outside the skin, which emits parents before
  // children, the invariant the whole model-space accumulation relies on
  auto build = SkinBuildData{};
  build.joint_to_bone.assign(joint_count, 0_u32);
  build.bone_to_node.reserve(joint_count);

  auto joint_parent_bone = std::vector<i32>();
  joint_parent_bone.reserve(joint_count);

  auto visited = std::vector<bool>(joint_count, false);
  auto stack = std::vector<std::pair<u32, i32>>(); // joint slot, parent bone index

  for (auto slot = 0_u32; slot < joint_count; ++slot) {
    const auto parent_node = node_parents[gltf_skin.joints[slot]];
    if (parent_node == ~0_sz || !node_to_joint.contains(parent_node)) {
      stack.emplace_back(slot, -1);
    }
  }

  if (stack.empty()) {
    OX_LOG_ERROR("Skin '{}' has no root joint; its joint hierarchy is cyclic.", std::string(gltf_skin.name));
    return nullopt;
  }

  while (!stack.empty()) {
    const auto [slot, parent_bone] = stack.back();
    stack.pop_back();

    if (visited[slot]) {
      continue;
    }
    visited[slot] = true;

    const auto bone = static_cast<u32>(build.bone_to_node.size());
    build.joint_to_bone[slot] = bone;
    build.bone_to_node.push_back(gltf_skin.joints[slot]);
    joint_parent_bone.push_back(parent_bone);

    const auto& node = gltf_asset.nodes[gltf_skin.joints[slot]];
    for (const auto child : std::views::reverse(node.children)) {
      if (const auto it = node_to_joint.find(static_cast<usize>(child)); it != node_to_joint.end()) {
        stack.emplace_back(it->second, static_cast<i32>(bone));
      }
    }
  }

  if (build.bone_to_node.size() != joint_count) {
    OX_LOG_ERROR("Skin '{}' has joints unreachable from any root joint.", std::string(gltf_skin.name));
    return nullopt;
  }

  // ancestors above the first root joint, which every root joint is assumed to share because a rig
  // with roots under different ancestors is malformed
  {
    auto prefix_chain = std::vector<usize>();
    auto node = node_parents[build.bone_to_node[0]];
    while (node != ~0_sz) {
      prefix_chain.push_back(node);
      node = node_parents[node];
    }

    for (const auto ancestor : std::views::reverse(prefix_chain)) {
      build.root_prefix = build.root_prefix * gltf_node_local_transform(gltf_asset.nodes[ancestor]);
    }
  }

  auto& skeleton = build.skeleton;
  skeleton.parent_indices = std::move(joint_parent_bone);
  skeleton.bone_names.reserve(joint_count);
  skeleton.parent_space_reference_pose.reserve(joint_count);
  skeleton.inverse_bind_pose.assign(joint_count, BoneTransform{});

  for (auto bone = 0_u32; bone < joint_count; ++bone) {
    const auto& node = gltf_asset.nodes[build.bone_to_node[bone]];
    auto name = std::string(node.name);
    skeleton.bone_names.emplace_back(name.empty() ? fmt::format("bone_{}", bone) : std::move(name));

    auto local = gltf_node_local_transform(node);
    if (skeleton.parent_indices[bone] < 0) {
      local = build.root_prefix * local;
    }
    skeleton.parent_space_reference_pose.emplace_back(local);
  }

  if (gltf_skin.inverseBindMatrices.has_value()) {
    const auto& accessor = gltf_asset.accessors[gltf_skin.inverseBindMatrices.value()];
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
      gltf_asset,
      accessor,
      [&](const fastgltf::math::fmat4x4& matrix, const usize slot) {
        if (slot < joint_count) {
          skeleton.inverse_bind_pose[build.joint_to_bone[slot]] = BoneTransform::from_mat4(
            glm::make_mat4(matrix.data())
          );
        }
      }
    );
  } else {
    // per spec the identity is the default, meaning the joints are already in mesh space
    for (auto bone = 0_u32; bone < joint_count; ++bone) {
      skeleton.inverse_bind_pose[bone] = BoneTransform{};
    }
  }

  if (!skeleton.finalize()) {
    return nullopt;
  }

  return build;
}

constexpr static auto ANIMATION_DEFAULT_FPS = 30.f;
constexpr static auto ANIMATION_MAX_FPS = 120.f;

struct GltfAnimationSampler {
  std::vector<f32> times = {};
  // vec3 channels use xyz, rotation uses all four
  std::vector<glm::vec4> values = {};
  fastgltf::AnimationInterpolation interpolation = fastgltf::AnimationInterpolation::Linear;

  auto sample(this const GltfAnimationSampler& self, f32 time, const glm::vec4& fallback) -> glm::vec4;
};

auto GltfAnimationSampler::sample(this const GltfAnimationSampler& self, const f32 time, const glm::vec4& fallback)
  -> glm::vec4 {
  const auto is_cubic = self.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
  const auto key_count = self.times.size();
  const auto value_at = [&](const usize key) {
    return self.values[is_cubic ? key * 3 + 1 : key];
  };

  if (key_count == 0 || self.values.size() < (is_cubic ? key_count * 3 : key_count)) {
    return fallback;
  }
  if (key_count == 1 || time <= self.times.front()) {
    return value_at(0);
  }
  if (time >= self.times.back()) {
    return value_at(key_count - 1);
  }

  const auto upper = static_cast<usize>(
    std::upper_bound(self.times.begin(), self.times.end(), time) - self.times.begin()
  );
  const auto lower = upper - 1;
  const auto span = self.times[upper] - self.times[lower];
  const auto t = span > glm::epsilon<f32>() ? (time - self.times[lower]) / span : 0.f;

  switch (self.interpolation) {
    case fastgltf::AnimationInterpolation::Step: {
      return value_at(lower);
    }
    case fastgltf::AnimationInterpolation::CubicSpline: {
      const auto p0 = self.values[lower * 3 + 1];
      const auto m0 = self.values[lower * 3 + 2] * span;
      const auto p1 = self.values[upper * 3 + 1];
      const auto m1 = self.values[upper * 3 + 0] * span;
      const auto t2 = t * t;
      const auto t3 = t2 * t;
      return (2.f * t3 - 3.f * t2 + 1.f) * p0 + (t3 - 2.f * t2 + t) * m0 + (-2.f * t3 + 3.f * t2) * p1 + (t3 - t2) * m1;
    }
    default: {
      return glm::mix(value_at(lower), value_at(upper), t);
    }
  }
}

auto read_gltf_animation_sampler(const fastgltf::Asset& gltf_asset, const fastgltf::AnimationSampler& gltf_sampler)
  -> GltfAnimationSampler {
  ZoneScoped;

  auto sampler = GltfAnimationSampler{.interpolation = gltf_sampler.interpolation};

  const auto& input = gltf_asset.accessors[gltf_sampler.inputAccessor];
  sampler.times.resize(input.count);
  fastgltf::iterateAccessorWithIndex<f32>(gltf_asset, input, [&](const f32 t, const usize i) { sampler.times[i] = t; });

  const auto& output = gltf_asset.accessors[gltf_sampler.outputAccessor];
  sampler.values.resize(output.count);
  if (output.type == fastgltf::AccessorType::Vec4) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf_asset, output, [&](const glm::vec4 v, const usize i) {
      sampler.values[i] = v;
    });
  } else if (output.type == fastgltf::AccessorType::Vec3) {
    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, output, [&](const glm::vec3 v, const usize i) {
      sampler.values[i] = glm::vec4(v, 0.f);
    });
  } else {
    sampler.values.clear();
  }

  return sampler;
}

struct BoneChannels {
  option<usize> translation = nullopt;
  option<usize> rotation = nullopt;
  option<usize> scale = nullopt;
};

auto build_gltf_animation(
  const fastgltf::Asset& gltf_asset,
  const fastgltf::Animation& gltf_animation,
  const SkinBuildData& skin,
  const UUID& skeleton_uuid
) -> option<AnimationClip> {
  ZoneScoped;

  const auto bone_count = skin.skeleton.bone_count();

  auto node_to_bone = ankerl::unordered_dense::map<usize, u32>();
  node_to_bone.reserve(bone_count);
  for (const auto& [node_index, bone] : std::views::zip(skin.bone_to_node, std::views::iota(0_u32))) {
    node_to_bone.emplace(node_index, bone);
  }

  auto samplers = std::vector<GltfAnimationSampler>();
  samplers.reserve(gltf_animation.samplers.size());
  for (const auto& gltf_sampler : gltf_animation.samplers) {
    samplers.emplace_back(read_gltf_animation_sampler(gltf_asset, gltf_sampler));
  }

  auto bone_channels = std::vector<BoneChannels>(bone_count);
  auto duration = 0.f;
  auto max_key_count = 0_sz;
  auto touched_bones = false;

  for (const auto& channel : gltf_animation.channels) {
    if (!channel.nodeIndex.has_value() || channel.samplerIndex >= samplers.size()) {
      continue;
    }

    const auto bone_it = node_to_bone.find(static_cast<usize>(channel.nodeIndex.value()));
    if (bone_it == node_to_bone.end()) {
      continue;
    }

    auto& channels = bone_channels[bone_it->second];
    switch (channel.path) {
      case fastgltf::AnimationPath::Translation: channels.translation = channel.samplerIndex; break;
      case fastgltf::AnimationPath::Rotation   : channels.rotation = channel.samplerIndex; break;
      case fastgltf::AnimationPath::Scale      : channels.scale = channel.samplerIndex; break;
      default                                  : continue;
    }

    const auto& sampler = samplers[channel.samplerIndex];
    if (!sampler.times.empty()) {
      duration = glm::max(duration, sampler.times.back());
      max_key_count = ox::max(max_key_count, sampler.times.size());
    }
    touched_bones = true;
  }

  if (!touched_bones || duration <= 0.f) {
    return nullopt;
  }

  // resample onto a fixed rate, which is what makes the frame-major layout possible at all, with
  // enough frames that a densely keyed clip does not lose detail
  auto fps = ANIMATION_DEFAULT_FPS;
  if (max_key_count > 1) {
    fps = glm::clamp(static_cast<f32>(max_key_count - 1) / duration, ANIMATION_DEFAULT_FPS, ANIMATION_MAX_FPS);
  }

  const auto frame_count = ox::max(2_u32, static_cast<u32>(glm::round(duration * fps)) + 1_u32);

  auto sampled = std::vector<BoneTransform>(static_cast<usize>(frame_count) * bone_count);
  for (auto frame = 0_u32; frame < frame_count; ++frame) {
    const auto time = duration * static_cast<f32>(frame) / static_cast<f32>(frame_count - 1);

    for (auto bone = 0_u32; bone < bone_count; ++bone) {
      const auto& reference = skin.skeleton.parent_space_reference_pose[bone];
      const auto is_root = skin.skeleton.parent_indices[bone] < 0;
      // glTF channels drive node-local values and the reference pose already carries the root
      // prefix, so strip it before sampling and put it back afterwards
      const auto local_reference = is_root ? skin.root_prefix.inverse() * reference : reference;

      auto& channels = bone_channels[bone];
      auto translation = local_reference.translation();
      auto rotation = local_reference.rotation;
      auto scale = local_reference.scale();

      if (channels.translation.has_value()) {
        translation = glm::vec3(samplers[channels.translation.value()].sample(time, glm::vec4(translation, 0.f)));
      }
      if (channels.rotation.has_value()) {
        const auto sampled_rotation = samplers[channels.rotation.value()].sample(
          time,
          glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w)
        );
        rotation = glm::normalize(
          glm::quat::wxyz(sampled_rotation.w, sampled_rotation.x, sampled_rotation.y, sampled_rotation.z)
        );
      }
      if (channels.scale.has_value()) {
        const auto sampled_scale = glm::vec3(samplers[channels.scale.value()].sample(time, glm::vec4(scale)));
        scale = (sampled_scale.x + sampled_scale.y + sampled_scale.z) / 3.f;
      }

      auto local = BoneTransform::from_trs(translation, rotation, scale);
      sampled[static_cast<usize>(frame) * bone_count + bone] = is_root ? skin.root_prefix * local : local;
    }
  }

  auto clip = AnimationClip{};
  clip.name = std::string(gltf_animation.name);
  clip.skeleton_uuid = skeleton_uuid;
  clip.frame_count = frame_count;
  clip.duration = duration;
  animation::compress_tracks(clip, sampled, bone_count, frame_count);

  return clip;
}

auto extract_skeleton_uuids(simdjson::ondemand::document& doc) -> std::vector<UUID> {
  ZoneScoped;

  auto result = std::vector<UUID>();
  auto array = doc["skeletons"].get_array();
  if (array.error()) {
    return result;
  }

  for (auto obj_json : array) {
    if (obj_json.error() || !obj_json.is_string()) {
      continue;
    }

    if (auto uuid = UUID::from_string(obj_json.get_string()); uuid.has_value()) {
      result.emplace_back(uuid.value());
    }
  }

  return result;
}

struct AnimationMeta {
  UUID uuid = UUID(nullptr);
  u32 animation_index = 0;
};

auto extract_animation_metas(simdjson::ondemand::document& doc) -> std::vector<AnimationMeta> {
  ZoneScoped;

  auto result = std::vector<AnimationMeta>();
  auto array = doc["animations"].get_array();
  if (array.error()) {
    return result;
  }

  for (auto obj_json : array) {
    if (obj_json.error()) {
      continue;
    }

    auto uuid_json = obj_json["uuid"].get_string();
    auto index_json = obj_json["animation_index"].get_uint64();
    if (uuid_json.error() || index_json.error()) {
      continue;
    }

    auto uuid = UUID::from_string(uuid_json.value_unsafe());
    if (!uuid.has_value()) {
      continue;
    }

    result.emplace_back(
      AnimationMeta{
        .uuid = uuid.value(),
        .animation_index = static_cast<u32>(index_json.value_unsafe()),
      }
    );
  }

  return result;
}

struct MeshBuildData {
  GPU::Mesh gpu_mesh = {};
  std::array<GPU::MeshLOD, GPU::Mesh::MAX_LODS> lods = {};
  std::vector<u8> blob = {};
  Model::CollisionMesh collision = {};
  u64 lod_metadata_offset = 0;
  bool has_texture_coords = false;
  bool has_skin = false;
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

// `joint_to_bone` maps a glTF skin joint slot to its index in the topologically sorted skeleton,
// and empty means the primitive is not skinned
auto build_gltf_mesh(
  const fastgltf::Asset& gltf_asset,
  const fastgltf::Primitive& gltf_primitive,
  std::span<const u32> joint_to_bone,
  std::span<const BoneTransform> inverse_bind_pose,
  f32& out_max_bone_influence_radius
) -> option<MeshBuildData> {
  ZoneScoped;

  const auto position_attrib = gltf_primitive.findAttribute("POSITION");
  if (position_attrib == gltf_primitive.attributes.end()) {
    return nullopt;
  }

  auto build = MeshBuildData{};

  auto raw_indices = std::vector<u32>();
  if (gltf_primitive.indicesAccessor.has_value()) {
    auto& index_accessor = gltf_asset.accessors[gltf_primitive.indicesAccessor.value()];
    raw_indices.resize(index_accessor.count);
    fastgltf::iterateAccessorWithIndex<u32>(gltf_asset, index_accessor, [&](u32 index, usize i) {
      raw_indices[i] = index;
    });
  } else {
    // glTF allows a non-indexed triangle soup but the rest of this pipeline does not, and a
    // sequential index buffer costs one pass while letting meshopt, the meshlet builder and the
    // BLAS work unchanged, leaving vertices unwelded so such a mesh simply gets no LOD chain
    raw_indices.resize(gltf_asset.accessors[position_attrib->accessorIndex].count);
    std::iota(raw_indices.begin(), raw_indices.end(), 0_u32);
  }

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

  auto joints = std::vector<glm::u16vec4>();
  auto weights = std::vector<glm::vec4>();
  if (!joint_to_bone.empty()) {
    const auto joints_attrib = gltf_primitive.findAttribute("JOINTS_0");
    const auto weights_attrib = gltf_primitive.findAttribute("WEIGHTS_0");
    if (joints_attrib != gltf_primitive.attributes.end() && weights_attrib != gltf_primitive.attributes.end()) {
      auto& joints_accessor = gltf_asset.accessors[joints_attrib->accessorIndex];
      auto raw_joints = std::vector<glm::u16vec4>(joints_accessor.count);
      fastgltf::iterateAccessorWithIndex<glm::u16vec4>(gltf_asset, joints_accessor, [&](glm::u16vec4 j, usize i) {
        raw_joints[i] = j;
      });

      auto& weights_accessor = gltf_asset.accessors[weights_attrib->accessorIndex];
      auto raw_weights = std::vector<glm::vec4>(weights_accessor.count);
      fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf_asset, weights_accessor, [&](glm::vec4 w, usize i) {
        raw_weights[i] = w;
      });

      joints.resize(vertex_count);
      meshopt_remapVertexBuffer(
        joints.data(),
        raw_joints.data(),
        raw_joints.size(),
        sizeof(glm::u16vec4),
        vertex_remap.data()
      );

      weights.resize(vertex_count);
      meshopt_remapVertexBuffer(
        weights.data(),
        raw_weights.data(),
        raw_weights.size(),
        sizeof(glm::vec4),
        vertex_remap.data()
      );
    }
  }

  auto indices = std::vector<u32>(raw_indices.size());
  meshopt_remapIndexBuffer(indices.data(), raw_indices.data(), raw_indices.size(), vertex_remap.data());

  if (normals.empty() && vertex_count > 0) {
    // NORMAL is optional in glTF and a mesh without it must be shaded flat, which accumulating
    // face normals gives exactly on an unwelded mesh, whereas leaving the array zeroed would decode
    // to (-1, -1, -1) in the shader and light the whole mesh from one wrong direction
    normals.assign(vertex_count, glm::vec3(0.f));
    for (auto i = 0_sz; i + 2 < indices.size(); i += 3) {
      const auto i0 = indices[i];
      const auto i1 = indices[i + 1];
      const auto i2 = indices[i + 2];
      const auto face = glm::cross(positions[i1] - positions[i0], positions[i2] - positions[i0]);
      normals[i0] += face;
      normals[i1] += face;
      normals[i2] += face;
    }

    for (auto& normal : normals) {
      normal = glm::dot(normal, normal) > 0.f ? glm::normalize(normal) : glm::vec3(0.f, 1.f, 0.f);
    }
  }

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

  build.collision.positions = positions;
  build.collision.indices = indices;

  auto quantized_joints = std::vector<glm::u16vec4>();
  auto quantized_weights = std::vector<glm::u16vec4>();
  if (!joints.empty()) {
    quantized_joints.resize(vertex_count);
    quantized_weights.resize(vertex_count);

    for (auto i = 0_u32; i < vertex_count; ++i) {
      // exporters routinely emit weights that sum to slightly off 1.0, and the shader does a plain
      // weighted sum with no correction of its own
      auto weight = weights[i];
      const auto sum = weight.x + weight.y + weight.z + weight.w;
      weight = sum > glm::epsilon<f32>() ? weight / sum : glm::vec4(1.f, 0.f, 0.f, 0.f);

      for (auto influence = 0_u32; influence < MAX_BONE_INFLUENCES; ++influence) {
        const auto joint = joints[i][static_cast<int>(influence)];
        const auto bone = joint < joint_to_bone.size() ? joint_to_bone[joint] : 0_u32;
        quantized_joints[i][static_cast<int>(influence)] = static_cast<u16>(bone);
        quantized_weights[i][static_cast<int>(influence)] = static_cast<u16>(
          glm::clamp(weight[static_cast<int>(influence)], 0.f, 1.f) * 65535.f + 0.5f
        );

        // widest bind-pose reach of any bone, which is what inflates the animated culling bounds
        if (weight[static_cast<int>(influence)] > 0.f && bone < inverse_bind_pose.size()) {
          const auto bone_local = inverse_bind_pose[bone].transform_point(positions[i]);
          out_max_bone_influence_radius = glm::max(out_max_bone_influence_radius, glm::length(bone_local));
        }
      }
    }
  }

  auto& gpu_mesh = build.gpu_mesh;
  gpu_mesh.vertex_count = vertex_count;
  gpu_mesh.vertex_positions = blob_append(build.blob, quantized_positions, 8);
  gpu_mesh.vertex_normals = blob_append(build.blob, quantized_normals, 4);
  if (!texcoords.empty()) {
    build.has_texture_coords = true;
    gpu_mesh.texture_coords = blob_append(build.blob, quantized_texcoords, 4);
  }
  if (!quantized_joints.empty()) {
    build.has_skin = true;
    gpu_mesh.skin_joint_indices = blob_append(build.blob, quantized_joints, 8);
    gpu_mesh.skin_weights = blob_append(build.blob, quantized_weights, 8);
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

auto upload_gltf_mesh(
  RenderContext& render_context, MeshBuildData& build, UploadBatch* batch, AccelerationStructure& out_blas
) -> vuk::Unique<vuk::Buffer> {
  ZoneScoped;

  auto gpu_buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, build.blob.size());
  const auto gpu_mesh_bda = gpu_buffer->device_address;

  build.gpu_mesh.vertex_positions += gpu_mesh_bda;
  build.gpu_mesh.vertex_normals += gpu_mesh_bda;
  if (build.has_texture_coords) {
    build.gpu_mesh.texture_coords += gpu_mesh_bda;
  }
  if (build.has_skin) {
    build.gpu_mesh.skin_joint_indices += gpu_mesh_bda;
    build.gpu_mesh.skin_weights += gpu_mesh_bda;
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
  auto upload = render_context.upload_staging(std::move(staging_value), std::move(gpu_mesh_value));

  auto blas_scratch = vuk::Unique<vuk::Buffer>();
  upload = build_mesh_blas(
    render_context,
    BLASBuildInfo{
      .vertex_positions = build.gpu_mesh.vertex_positions,
      .indices = build.lods[0].indices,
      .vertex_count = build.gpu_mesh.vertex_count,
      .index_count = build.lods[0].indices_count,
    },
    std::move(upload),
    out_blas,
    blas_scratch
  );

  if (batch) {
    auto values = std::array<vuk::UntypedValue, 1>{std::move(upload)};
    render_context.submit_multiple(values);
    batch->add_upload(values);
    auto owned = std::array<vuk::Unique<vuk::Buffer>, 2>{std::move(staging_buffer), std::move(blas_scratch)};
    batch->take_staging(owned);
  } else {
    render_context.wait_on(std::move(upload));
  }

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

    auto job = Job::create(std::forward<decltype(work)>(work));
    job->signal(barrier);
    job_man.submit(std::move(job));
  };

  auto& render_context = App::get_rendercontext();

  OX_ASSERT(gltf_asset.textures.size() == textures.size());
  auto texture_barrier = Barrier::create();
  auto texture_batch = UploadBatch::create();
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
       is_srgb,
       texture_batch]() {
        load_gltf_texture(
          asset_man,
          *gltf_asset_ref,
          path,
          texture_uuid,
          gltf_asset_ref->images[gltf_image_index],
          gltf_asset_ref->textures[texture_index],
          is_srgb,
          texture_batch.get()
        );
      }
    );
  }

  texture_barrier->wait(job_man);
  // One fence wait and one vkUpdateDescriptorSets for every texture in the model. Has to happen
  // before the materials below reach the GPU, since they index the slots written here.
  texture_batch->flush(render_context);

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

  // a clip names the nodes it drives, so more than one skin makes "which skeleton does this clip
  // target" ambiguous, mirroring the single-scene restriction above
  if (gltf_asset.skins.size() > 1) {
    OX_LOG_ERROR("Model '{}' has {} skins; only one is supported.", path, gltf_asset.skins.size());
    return ModelID::Invalid;
  }

  auto skin = option<SkinBuildData>(nullopt);
  auto skeleton_uuid = UUID(nullptr);
  // shared, because the mesh build jobs outlive this function on the async path
  auto joint_to_bone = std::make_shared<std::vector<u32>>();
  auto inverse_bind_pose = std::make_shared<std::vector<BoneTransform>>();
  auto animation_uuids = std::vector<UUID>();
  auto joint_nodes = ankerl::unordered_dense::set<usize>();

  if (!gltf_asset.skins.empty()) {
    const auto skeleton_uuids = extract_skeleton_uuids(*meta_json->doc);
    if (skeleton_uuids.empty()) {
      // meta file predates skeleton support, and minting a UUID here would produce a different one
      // every run and break every scene that referenced it, so load the model as static instead
      OX_LOG_WARN(
        "Model '{}' has a skin but its meta file lists no skeleton UUID. Loading it as static; delete "
        "the .oxasset and re-import to get animation.",
        path
      );
    } else {
      skin = build_gltf_skeleton(gltf_asset, gltf_asset.skins[0]);
      if (!skin.has_value()) {
        OX_LOG_ERROR("Failed to build the skeleton of '{}'.", path);
        return ModelID::Invalid;
      }

      skeleton_uuid = skeleton_uuids[0];
    }
  }

  if (skin.has_value()) {
    for (const auto node_index : skin->bone_to_node) {
      joint_nodes.emplace(node_index);
    }

    self.register_asset(skeleton_uuid, AssetType::Skeleton, path);
    joint_to_bone = std::make_shared<std::vector<u32>>(skin->joint_to_bone);
    inverse_bind_pose = std::make_shared<std::vector<BoneTransform>>(skin->skeleton.inverse_bind_pose);

    for (const auto& animation_meta : extract_animation_metas(*meta_json->doc)) {
      if (animation_meta.animation_index >= gltf_asset.animations.size()) {
        continue;
      }

      auto clip = build_gltf_animation(
        gltf_asset,
        gltf_asset.animations[animation_meta.animation_index],
        skin.value(),
        skeleton_uuid
      );
      if (!clip.has_value()) {
        continue;
      }

      self.register_asset(animation_meta.uuid, AssetType::Animation, path);
      self.publish_animation(animation_meta.uuid, std::move(clip.value()));
      animation_uuids.emplace_back(animation_meta.uuid);
    }

    self.publish_skeleton(skeleton_uuid, std::move(skin->skeleton));
  }

  auto model = Model{};
  model.textures = std::move(textures);
  model.materials = std::move(materials);
  model.lights = std::move(lights);
  model.skeleton_uuid = skeleton_uuid;
  model.animations = std::move(animation_uuids);

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

    // joints live in the Skeleton, not the scene graph, because an entity per joint would make
    // hundreds of them per character whose transforms would fight the pose every frame
    if (joint_nodes.contains(gltf_node_index)) {
      continue;
    }

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
      if (gltf_primitive.findAttribute("POSITION") == gltf_primitive.attributes.end()) {
        continue;
      }

      auto mesh_index = model.gpu_meshes.size();
      mesh_group.mesh_indices.push_back(mesh_index);

      auto mesh_material_index = option<u32>(nullopt);
      if (gltf_primitive.materialIndex.has_value()) {
        mesh_material_index = static_cast<u32>(gltf_primitive.materialIndex.value());
      }

      model.material_indices.push_back(mesh_material_index);
      model.gpu_meshes.emplace_back();
      model.lod0_meshlet_counts.push_back(0_u32);
      model.index_ranges.emplace_back();
      model.gpu_mesh_buffers.emplace_back();
      model.mesh_blases.emplace_back();
      model.collision_meshes.emplace_back();
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

  auto mesh_barrier = Barrier::create();
  auto mesh_batch = UploadBatch::create();
  for (const auto& [pending_mesh, mesh_index] : std::views::zip(pending_meshes, std::views::iota(0_sz))) {
    dispatch(
      mesh_barrier,
      [&asset_man = self,
       model_id,
       gltf_asset_ref,
       &render_context,
       pending_mesh,
       mesh_index,
       mesh_batch,
       joint_to_bone,
       inverse_bind_pose]() {
        ZoneScopedN("GLTF Mesh Build");

        const auto& gltf_primitive = gltf_asset_ref->meshes[pending_mesh.gltf_mesh_index]
                                       .primitives[pending_mesh.gltf_primitive_index];
        auto max_bone_influence_radius = 0.f;
        auto build = build_gltf_mesh(
          *gltf_asset_ref,
          gltf_primitive,
          *joint_to_bone,
          *inverse_bind_pose,
          max_bone_influence_radius
        );
        auto mesh_buffer = vuk::Unique<vuk::Buffer>();
        auto mesh_blas = AccelerationStructure();
        if (build) {
          mesh_buffer = upload_gltf_mesh(render_context, *build, mesh_batch.get(), mesh_blas);
        }

        auto loaded_model = asset_man.get_model(model_id);
        if (!loaded_model) {
          // Still notify, or every waiter on this id blocks forever.
          mesh_batch->flush(render_context);
          asset_man.notify_model_loaded();
          return;
        }

        if (build) {
          loaded_model->gpu_mesh_buffers[mesh_index] = std::move(mesh_buffer);
          loaded_model->mesh_blases[mesh_index] = std::move(mesh_blas);
          loaded_model->gpu_meshes[mesh_index] = build->gpu_mesh;
          loaded_model->collision_meshes[mesh_index] = std::move(build->collision);
          loaded_model->lod0_meshlet_counts[mesh_index] = build->lods[0].meshlet_count;
          auto& index_ranges = loaded_model->index_ranges[mesh_index];
          index_ranges.lod_count = build->gpu_mesh.lod_count;
          for (auto lod_index = 0_u32; lod_index < index_ranges.lod_count; lod_index++) {
            index_ranges.lods[lod_index] = {build->lods[lod_index].indices, build->lods[lod_index].indices_count};
          }

          auto radius_ref = std::atomic_ref(loaded_model->max_bone_influence_radius);
          auto known = radius_ref.load(std::memory_order_relaxed);
          while (known < max_bone_influence_radius &&
                 !radius_ref.compare_exchange_weak(known, max_bone_influence_radius, std::memory_order_relaxed)) {
          }

          loaded_model->mesh_ready[mesh_index].test_and_set(std::memory_order_release);
        }

        auto pending = std::atomic_ref(loaded_model->pending_meshes);
        const auto was_last = pending.fetch_sub(1, std::memory_order_acq_rel) == 1;
        // Lock order is model_load -> models, so drop `models_mutex` before notifying.
        loaded_model.reset();

        if (was_last) {
          // Nothing waits on `mesh_barrier` when async, so the last mesh in has to settle the batch
          // before the model is announced as loaded.
          mesh_batch->flush(render_context);
          asset_man.notify_model_loaded();
        }
      }
    );
  }

  if (!async) {
    mesh_barrier->wait(job_man);
  }

  return model_id;
}

auto AssetManager::load_model(this AssetManager& self, const ModelLoadInfo& info) -> ModelID {
  ZoneScoped;

  const auto vertex_count = static_cast<u32>(info.vertices.size());
  if (vertex_count == 0 || info.indices.empty()) {
    OX_LOG_ERROR("Cannot load a model from empty vertices or indices!");
    return ModelID::Invalid;
  }

  auto& render_context = App::get_rendercontext();

  auto model = Model{.materials = info.materials};

  for (const auto& material_uuid : model.materials) {
    self.load_asset(material_uuid, {}, false);
  }

  auto quantized_positions = std::vector<glm::u16vec4>(vertex_count);
  auto quantized_normals = std::vector<u32>(vertex_count);
  auto quantized_texcoords = std::vector<glm::u16vec2>(vertex_count);
  auto position_floats = std::vector<f32>(vertex_count * 3);

  for (u32 i = 0; i < vertex_count; ++i) {
    const auto& vertex = info.vertices[i];

    quantized_positions[i] = glm::u16vec4(
      meshopt_quantizeHalf(vertex.position.x),
      meshopt_quantizeHalf(vertex.position.y),
      meshopt_quantizeHalf(vertex.position.z),
      0
    );
    quantized_normals[i] = ((meshopt_quantizeSnorm(vertex.normal.x, 10) + 511) << 20) |
                           ((meshopt_quantizeSnorm(vertex.normal.y, 10) + 511) << 10) |
                           (meshopt_quantizeSnorm(vertex.normal.z, 10) + 511);
    quantized_texcoords[i] = glm::u16vec2(meshopt_quantizeHalf(vertex.uv.x), meshopt_quantizeHalf(vertex.uv.y));

    position_floats[i * 3 + 0] = vertex.position.x;
    position_floats[i * 3 + 1] = vertex.position.y;
    position_floats[i * 3 + 2] = vertex.position.z;
  }

  const auto max_meshlets = meshopt_buildMeshletsBound(
    info.indices.size(),
    Model::MAX_MESHLET_INDICES,
    Model::MAX_MESHLET_PRIMITIVES
  );

  auto raw_meshlets = std::vector<meshopt_Meshlet>(max_meshlets);
  auto indirect_vertex_indices = std::vector<u32>(max_meshlets * Model::MAX_MESHLET_INDICES);
  auto local_triangle_indices = std::vector<u8>(max_meshlets * Model::MAX_MESHLET_PRIMITIVES * 3);

  const auto meshlet_count = meshopt_buildMeshlets(
    raw_meshlets.data(),
    indirect_vertex_indices.data(),
    local_triangle_indices.data(),
    info.indices.data(),
    info.indices.size(),
    position_floats.data(),
    vertex_count,
    sizeof(glm::vec3),
    Model::MAX_MESHLET_INDICES,
    Model::MAX_MESHLET_PRIMITIVES,
    0.0f
  );

  if (meshlet_count == 0) {
    OX_LOG_ERROR("Failed to build meshlets for a procedural model!");
    return ModelID::Invalid;
  }

  raw_meshlets.resize(meshlet_count);
  auto meshlets = std::vector<GPU::Meshlet>(meshlet_count);
  auto gpu_meshlet_bounds = std::vector<GPU::MeshletBounds>(meshlet_count);

  const auto& last_meshlet = raw_meshlets[meshlet_count - 1];
  indirect_vertex_indices.resize(last_meshlet.vertex_offset + last_meshlet.vertex_count);
  local_triangle_indices.resize(last_meshlet.triangle_offset + ((last_meshlet.triangle_count * 3 + 3) & ~3_u32));

  auto mesh_bb_min = glm::vec3(std::numeric_limits<f32>::max());
  auto mesh_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());

  for (usize i = 0; i < meshlet_count; ++i) {
    const auto& raw_meshlet = raw_meshlets[i];
    auto& meshlet = meshlets[i];
    auto& bounds = gpu_meshlet_bounds[i];

    auto meshlet_bb_min = glm::vec3(std::numeric_limits<f32>::max());
    auto meshlet_bb_max = glm::vec3(std::numeric_limits<f32>::lowest());

    for (u32 tri_index = 0; tri_index < raw_meshlet.triangle_count * 3; ++tri_index) {
      const auto local_index = local_triangle_indices[raw_meshlet.triangle_offset + tri_index];
      const auto indirect_index = indirect_vertex_indices[raw_meshlet.vertex_offset + local_index];
      const auto& position = info.vertices[indirect_index].position;

      meshlet_bb_min = glm::min(meshlet_bb_min, position);
      meshlet_bb_max = glm::max(meshlet_bb_max, position);
    }

    const auto meshopt_bounds = meshopt_computeMeshletBounds(
      &indirect_vertex_indices[raw_meshlet.vertex_offset],
      &local_triangle_indices[raw_meshlet.triangle_offset],
      raw_meshlet.triangle_count,
      position_floats.data(),
      vertex_count,
      sizeof(glm::vec3)
    );

    const auto meshlet_center = (meshlet_bb_max + meshlet_bb_min) * 0.5f;
    const auto meshlet_extent = meshlet_bb_max - meshlet_bb_min;

    meshlet.indirect_vertex_index_offset = raw_meshlet.vertex_offset;
    meshlet.local_triangle_index_offset = raw_meshlet.triangle_offset;
    meshlet.vertex_count = raw_meshlet.vertex_count;
    meshlet.triangle_count = raw_meshlet.triangle_count;

    bounds.aabb_center = {
      meshopt_quantizeHalf(meshlet_center.x),
      meshopt_quantizeHalf(meshlet_center.y),
      meshopt_quantizeHalf(meshlet_center.z)
    };
    bounds.aabb_extent = {
      meshopt_quantizeHalf(meshlet_extent.x),
      meshopt_quantizeHalf(meshlet_extent.y),
      meshopt_quantizeHalf(meshlet_extent.z)
    };
    bounds.cone_axis_xy = {meshopt_bounds.cone_axis_s8[0], meshopt_bounds.cone_axis_s8[1]};
    bounds.cone_axis_z = meshopt_bounds.cone_axis_s8[2];
    bounds.cone_cutoff = meshopt_bounds.cone_cutoff_s8;

    mesh_bb_min = glm::min(mesh_bb_min, meshlet_bb_min);
    mesh_bb_max = glm::max(mesh_bb_max, meshlet_bb_max);
  }

  const auto mesh_vertices_size = ox::align_up(
    ox::size_bytes(quantized_positions) + ox::size_bytes(quantized_normals) + ox::size_bytes(quantized_texcoords),
    8
  );

  auto lod0_size = 0_u64;
  lod0_size += ox::size_bytes(info.indices);
  lod0_size = ox::align_up(lod0_size, 8);
  lod0_size += ox::size_bytes(meshlets);
  lod0_size = ox::align_up(lod0_size, 8);
  lod0_size += ox::size_bytes(gpu_meshlet_bounds);
  lod0_size = ox::align_up(lod0_size, 8);
  lod0_size += ox::size_bytes(local_triangle_indices);
  lod0_size = ox::align_up(lod0_size, 4);
  lod0_size += ox::size_bytes(indirect_vertex_indices);
  lod0_size = ox::align_up(lod0_size, 8); // pad end so the LOD metadata base stays 8-aligned

  const auto lod_metadata_size = ox::align_up(sizeof(GPU::MeshLOD), 8);
  const auto total_gpu_size = mesh_vertices_size + lod0_size + lod_metadata_size;

  auto gpu_mesh_buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, total_gpu_size);
  const auto gpu_bda = gpu_mesh_buffer->device_address;

  auto gpu_mesh = GPU::Mesh{};
  gpu_mesh.vertex_count = vertex_count;
  gpu_mesh.lod_count = 1;
  gpu_mesh.bounds = {
    .aabb_center = (mesh_bb_max + mesh_bb_min) * 0.5f,
    .aabb_extent = mesh_bb_max - mesh_bb_min,
  };

  auto cpu_vertex_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, mesh_vertices_size);
  auto* cpu_vertex_ptr = reinterpret_cast<u8*>(cpu_vertex_buffer->mapped_ptr);

  auto vertex_offset = 0_u64;

  gpu_mesh.vertex_positions = gpu_bda + vertex_offset;
  std::memcpy(cpu_vertex_ptr + vertex_offset, quantized_positions.data(), ox::size_bytes(quantized_positions));
  vertex_offset += ox::size_bytes(quantized_positions);

  gpu_mesh.vertex_normals = gpu_bda + vertex_offset;
  std::memcpy(cpu_vertex_ptr + vertex_offset, quantized_normals.data(), ox::size_bytes(quantized_normals));
  vertex_offset += ox::size_bytes(quantized_normals);

  gpu_mesh.texture_coords = gpu_bda + vertex_offset;
  std::memcpy(cpu_vertex_ptr + vertex_offset, quantized_texcoords.data(), ox::size_bytes(quantized_texcoords));

  auto vertex_subrange = vuk::discard_buf("mesh vertices", gpu_mesh_buffer->subrange(0, mesh_vertices_size));
  render_context.wait_on(render_context.upload_staging(std::move(cpu_vertex_buffer), std::move(vertex_subrange)));

  auto cpu_lod0_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, lod0_size);
  auto* cpu_lod0_ptr = reinterpret_cast<u8*>(cpu_lod0_buffer->mapped_ptr);

  auto lod0 = GPU::MeshLOD{};
  auto write_offset = 0_u64;

  lod0.indices = write_offset;
  std::memcpy(cpu_lod0_ptr + write_offset, info.indices.data(), ox::size_bytes(info.indices));
  write_offset += ox::size_bytes(info.indices);
  write_offset = ox::align_up(write_offset, 8);

  lod0.meshlets = write_offset;
  std::memcpy(cpu_lod0_ptr + write_offset, meshlets.data(), ox::size_bytes(meshlets));
  write_offset += ox::size_bytes(meshlets);
  write_offset = ox::align_up(write_offset, 8);

  lod0.meshlet_bounds = write_offset;
  std::memcpy(cpu_lod0_ptr + write_offset, gpu_meshlet_bounds.data(), ox::size_bytes(gpu_meshlet_bounds));
  write_offset += ox::size_bytes(gpu_meshlet_bounds);
  write_offset = ox::align_up(write_offset, 8);

  lod0.local_triangle_indices = write_offset;
  std::memcpy(cpu_lod0_ptr + write_offset, local_triangle_indices.data(), ox::size_bytes(local_triangle_indices));
  write_offset += ox::size_bytes(local_triangle_indices);
  write_offset = ox::align_up(write_offset, 4);

  lod0.indirect_vertex_indices = write_offset;
  std::memcpy(cpu_lod0_ptr + write_offset, indirect_vertex_indices.data(), ox::size_bytes(indirect_vertex_indices));

  lod0.indices_count = static_cast<u32>(info.indices.size());
  lod0.meshlet_count = static_cast<u32>(meshlet_count);
  lod0.meshlet_bounds_count = static_cast<u32>(gpu_meshlet_bounds.size());
  lod0.local_triangle_indices_count = static_cast<u32>(local_triangle_indices.size());
  lod0.indirect_vertex_indices_count = static_cast<u32>(indirect_vertex_indices.size());

  const auto lod0_bda = gpu_bda + mesh_vertices_size;
  lod0.indices += lod0_bda;
  lod0.meshlets += lod0_bda;
  lod0.meshlet_bounds += lod0_bda;
  lod0.local_triangle_indices += lod0_bda;
  lod0.indirect_vertex_indices += lod0_bda;

  auto lod0_subrange = vuk::discard_buf("mesh lod0", gpu_mesh_buffer->subrange(mesh_vertices_size, lod0_size));
  render_context.wait_on(render_context.upload_staging(std::move(cpu_lod0_buffer), std::move(lod0_subrange)));

  const auto metadata_offset = mesh_vertices_size + lod0_size;
  gpu_mesh.lods = gpu_bda + metadata_offset;

  auto cpu_metadata_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, lod_metadata_size);
  std::memcpy(cpu_metadata_buffer->mapped_ptr, &lod0, sizeof(GPU::MeshLOD));

  auto metadata_subrange = vuk::discard_buf(
    "mesh lod metadata",
    gpu_mesh_buffer->subrange(metadata_offset, lod_metadata_size)
  );
  render_context.wait_on(render_context.upload_staging(std::move(cpu_metadata_buffer), std::move(metadata_subrange)));

  auto mesh_blas = AccelerationStructure();
  {
    // The scratch dies with this scope, after the build it feeds has been waited on.
    auto blas_scratch = vuk::Unique<vuk::Buffer>();
    auto mesh_value = vuk::acquire_buf("mesh", *gpu_mesh_buffer, vuk::Access::eMemoryRead);
    auto blas_value = build_mesh_blas(
      render_context,
      BLASBuildInfo{
        .vertex_positions = gpu_mesh.vertex_positions,
        .indices = lod0.indices,
        .vertex_count = gpu_mesh.vertex_count,
        .index_count = lod0.indices_count,
      },
      std::move(mesh_value),
      mesh_blas,
      blas_scratch
    );
    if (mesh_blas) {
      render_context.wait_on(std::move(blas_value));
    }
  }

  auto& root_group = model.mesh_groups.emplace_back();
  root_group.name = "Root";
  root_group.mesh_indices.push_back(0);

  auto& collision = model.collision_meshes.emplace_back();
  collision.positions.reserve(vertex_count);
  for (const auto& vertex : info.vertices) {
    collision.positions.push_back(vertex.position);
  }
  collision.indices = info.indices;

  model.gpu_meshes.push_back(gpu_mesh);
  model.gpu_mesh_buffers.push_back(std::move(gpu_mesh_buffer));
  model.mesh_blases.push_back(std::move(mesh_blas));
  model.lod0_meshlet_counts.push_back(lod0.meshlet_count);
  model.index_ranges.push_back({.lods = {Model::IndexRange{lod0.indices, lod0.indices_count}}, .lod_count = 1});
  model.material_indices.push_back(info.materials.empty() ? option<u32>(nullopt) : option<u32>(0));

  model.mesh_ready = std::vector<std::atomic_flag>(1);
  model.mesh_ready[0].test_and_set(std::memory_order_release);

  auto write_lock = std::unique_lock(self.models_mutex);
  return self.model_map.create_slot(std::move(model));
}

auto AssetManager::unload_model(this AssetManager& self, const ModelID model_id) -> bool {
  ZoneScoped;

  self.wait_until_model_loaded(model_id);

  auto write_lock = std::unique_lock(self.models_mutex);
  if (auto* model = self.model_map.slot(model_id)) {
    *model = Model{};
  }
  self.model_map.destroy_slot(model_id);

  return true;
}

} // namespace ox
