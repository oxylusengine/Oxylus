#include "ParserGLTF.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include "Core/Option.hpp"

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};

namespace ox {
static auto get_default_extensions() -> fastgltf::Extensions {
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

static auto get_default_options() -> fastgltf::Options {
  auto options = fastgltf::Options::None;
  options |= fastgltf::Options::LoadExternalBuffers;

  return options;
}

static auto to_vuk_filter(fastgltf::Filter f) -> vuk::Filter {
  switch (f) {
    case fastgltf::Filter::Nearest: return vuk::Filter::eNearest;
    default                       : return vuk::Filter::eLinear;
  }
}

static auto to_vuk_sampler_address_mode(fastgltf::Wrap w) -> vuk::SamplerAddressMode {
  switch (w) {
    case fastgltf::Wrap::ClampToEdge   : return vuk::SamplerAddressMode::eClampToEdge;
    case fastgltf::Wrap::MirroredRepeat: return vuk::SamplerAddressMode::eMirroredRepeat;
    case fastgltf::Wrap::Repeat        : return vuk::SamplerAddressMode::eRepeat;
  }
}

static auto to_asset_file_type(fastgltf::MimeType mime) -> AssetFileType {
  switch (mime) {
    case fastgltf::MimeType::JPEG: return AssetFileType::JPEG;
    case fastgltf::MimeType::PNG : return AssetFileType::PNG;
    case fastgltf::MimeType::KTX2: return AssetFileType::KTX2;
    default                      : return AssetFileType::None;
  }
}

auto GLTFMeshInfo::parse(const ::fs::path& path, GLTFMeshCallbacks callbacks) -> ox::option<GLTFMeshInfo> {
  ZoneScoped;

  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
  if (!gltf_buffer) {
    OX_LOG_ERROR("Couldn't read gltf data {}", fastgltf::getErrorMessage(gltf_buffer.error()));
    return ox::nullopt;
  }

  fastgltf::Parser parser(get_default_extensions());
  auto result = parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_options(), fastgltf::Category::All);
  if (!result) {
    OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(result.error()));
    return ox::nullopt;
  }

  fastgltf::Asset asset = std::move(result.get());

#ifdef OX_DEBUG
  auto asset_result = fastgltf::validate(asset);
  if (asset_result != fastgltf::Error::None) {
    OX_LOG_ERROR("Validation failed for GTLF asset! {}", fastgltf::getErrorMessage(asset_result));
    return ox::nullopt;
  }
#endif

  GLTFMeshInfo model = {};

  ///////////////////////////////////////////////
  // Buffers
  ///////////////////////////////////////////////

  std::vector<std::span<u8>> buffers = {};

  // sources::Vector is not used for importing, ignore it
  for (const auto& v : asset.buffers) {
    auto visitor = fastgltf::visitor{
        [](const auto&) {},
        [&](const fastgltf::sources::ByteView& view) {
          // Embedded byte
          buffers.emplace_back(std::bit_cast<u8*>(view.bytes.data()), view.bytes.size_bytes());
        },
        [&](const fastgltf::sources::Array& arr) {
          // Embedded array
          buffers.emplace_back(std::bit_cast<u8*>(arr.bytes.data()), arr.bytes.size_bytes());
        },
    };
    std::visit(visitor, v.data);
  }

  ///////////////////////////////////////////////
  // Samplers
  ///////////////////////////////////////////////

  for (const auto& v : asset.samplers) {
    auto& sampler = model.samplers.emplace_back();
    sampler.mag_filter = to_vuk_filter(v.magFilter.value_or(fastgltf::Filter::Linear));
    sampler.min_filter = to_vuk_filter(v.minFilter.value_or(fastgltf::Filter::Linear));
    sampler.address_u = to_vuk_sampler_address_mode(v.wrapS);
    sampler.address_v = to_vuk_sampler_address_mode(v.wrapT);
  }

  ///////////////////////////////////////////////
  // Images
  ///////////////////////////////////////////////

  for (const auto& v : asset.images) {
    auto visitor = fastgltf::visitor{
        [](const auto&) {},
        [&](const fastgltf::sources::ByteView& view) {
          // Embedded buffer
          std::vector<u8> pixels(view.bytes.size_bytes());
          std::memcpy(pixels.data(), view.bytes.data(), view.bytes.size_bytes());

          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<std::vector<u8>>(std::move(pixels));
          image_info.file_type = to_asset_file_type(view.mimeType);
        },
        [&](const fastgltf::sources::BufferView& view) {
          // Embedded buffer
          auto& buffer_view = asset.bufferViews[view.bufferViewIndex];
          auto& buffer = buffers[buffer_view.bufferIndex];

          std::vector<u8> pixels(buffer_view.byteLength);
          std::memcpy(pixels.data(), buffer.data() + buffer_view.byteOffset, buffer_view.byteLength);

          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<std::vector<u8>>(std::move(pixels));
          image_info.file_type = to_asset_file_type(view.mimeType);
        },
        [&](const fastgltf::sources::Array& arr) {
          // Embedded array
          std::vector<u8> pixels(arr.bytes.size_bytes());
          std::memcpy(pixels.data(), arr.bytes.data(), arr.bytes.size_bytes());

          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<std::vector<u8>>(std::move(pixels));
          image_info.file_type = to_asset_file_type(arr.mimeType);
        },
        [&](const fastgltf::sources::URI& uri) {
          // External file
          const auto& image_file_path = path.parent_path() / uri.uri.fspath();

          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<fs::path>(image_file_path);
          image_info.file_type = to_asset_file_type(uri.mimeType);
        },
    };
    std::visit(visitor, v.data);
  }

  ///////////////////////////////////////////////
  // Textures
  ///////////////////////////////////////////////

  for (const auto& v : asset.textures) {
    auto& texture = model.textures.emplace_back();
    if (v.samplerIndex.has_value()) {
      texture.sampler_index = v.samplerIndex.value();
    }

    if (v.imageIndex.has_value()) {
      texture.image_index = v.imageIndex.value();
    } else if (v.basisuImageIndex.has_value()) {
      texture.image_index = v.basisuImageIndex.value();
    }
  }

  ///////////////////////////////////////////////
  // Materials
  ///////////////////////////////////////////////

  for (const auto& v : asset.materials) {
    auto& material = model.materials.emplace_back();
    auto& pbr = v.pbrData;

    material.albedo_color = {
        pbr.baseColorFactor[0],
        pbr.baseColorFactor[1],
        pbr.baseColorFactor[2],
        pbr.baseColorFactor[3],
    };
    material.metallic_factor = pbr.metallicFactor;
    material.roughness_factor = pbr.roughnessFactor;
    material.emissive_color = {
        v.emissiveFactor[0],
        v.emissiveFactor[1],
        v.emissiveFactor[2],
        v.emissiveStrength,
    };
    material.alpha_mode = static_cast<GLTFAlphaMode>(v.alphaMode);
    material.alpha_cutoff = v.alphaCutoff;

    if (auto& tex = pbr.baseColorTexture; tex.has_value()) {
      material.albedo_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.normalTexture; tex.has_value()) {
      material.normal_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.emissiveTexture; tex.has_value()) {
      material.emissive_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.pbrData.metallicRoughnessTexture; tex.has_value()) {
      material.metallic_roughness_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.occlusionTexture; tex.has_value()) {
      material.occlusion_texture_index = tex->textureIndex;
    }
  }

  if (callbacks.on_materials_load) {
    callbacks.on_materials_load(model.materials, model.textures, model.images);
  }

  ///////////////////////////////////////////////
  // Scene Hierarchy
  ///////////////////////////////////////////////

  for (const auto& node : asset.nodes) {
    glm::vec3 translation = {};
    glm::quat rotation = {};
    glm::vec3 scale = {};
    if (auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
      translation = glm::make_vec3(trs->translation.data());
      rotation = glm::quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
      scale = glm::make_vec3(trs->scale.data());
    } else if (auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
      fastgltf::math::fvec3 scale_array{};
      fastgltf::math::fquat rotation_array{};
      fastgltf::math::fvec3 translation_array{};
      fastgltf::math::decomposeTransformMatrix(*mat, scale_array, rotation_array, translation_array);

      translation = glm::make_vec3(translation_array.data());
      rotation = glm::quat(rotation_array[3], rotation_array[0], rotation_array[1], rotation_array[2]);
      scale = glm::make_vec3(scale_array.data());
    }

    ox::option<usize> mesh_index = ox::nullopt;
    if (node.meshIndex.has_value()) {
      mesh_index = node.meshIndex.value();
    }

    model.nodes.push_back({.name = std::string(node.name.begin(), node.name.end()),
                           .mesh_index = std::move(mesh_index),
                           .children = std::vector(node.children.begin(), node.children.end()),
                           .translation = translation,
                           .rotation = rotation,
                           .scale = scale});
  }

  if (asset.defaultScene.has_value()) {
    model.defualt_scene_index = asset.defaultScene.value();
  }

  for (const auto& scene : asset.scenes) {
    model.scenes.push_back({.name = std::string(scene.name.begin(), scene.name.end()),
                            .node_indices = std::vector(scene.nodeIndices.begin(), scene.nodeIndices.end())});
  }

  ///////////////////////////////////////////////
  // Geometry
  ///////////////////////////////////////////////

  u32 global_mesh_index = 0;
  u32 global_vertex_offset = 0;
  u32 global_index_offset = 0;
  for (const auto& mesh : asset.meshes) {
    auto mesh_index = global_mesh_index++;
    for (const auto& primitive : mesh.primitives) {
      if (!primitive.materialIndex.has_value()) {
        continue;
      }

      auto position_attrib = primitive.findAttribute("POSITION");
      if (position_attrib == primitive.attributes.end()) {
        continue;
      }

      auto& position_accessor = asset.accessors[position_attrib->accessorIndex];
      auto& index_accessor = asset.accessors[primitive.indicesAccessor.value()];
      u32 primitive_vertex_count = static_cast<u32>(position_accessor.count);
      u32 primitive_index_count = static_cast<u32>(index_accessor.count);

      if (callbacks.on_new_primitive) {
        callbacks.on_new_primitive(callbacks.user_data,
                                   mesh_index,
                                   static_cast<u32>(primitive.materialIndex.value()),
                                   global_vertex_offset,
                                   primitive_vertex_count,
                                   global_index_offset,
                                   primitive_index_count);
      }

      if (callbacks.on_access_index) {
        fastgltf::iterateAccessorWithIndex<u32>(asset, index_accessor, [&](u32 index, usize i) { //
          callbacks.on_access_index(callbacks.user_data, mesh_index, global_index_offset + i, index);
        });
      }

      if (callbacks.on_access_position) {
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, position_accessor, [&](glm::vec3 pos, usize i) { //
          callbacks.on_access_position(callbacks.user_data, mesh_index, global_vertex_offset + i, pos);
        });
      }

      if (auto attrib = primitive.findAttribute("NORMAL");
          attrib != primitive.attributes.end() && callbacks.on_access_normal) {
        auto& accessor = asset.accessors[attrib->accessorIndex];
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, accessor, [&](glm::vec3 normal, usize i) { //
          callbacks.on_access_normal(callbacks.user_data, mesh_index, global_vertex_offset + i, normal);
        });
      }

      if (auto attrib = primitive.findAttribute("TEXCOORD_0");
          attrib != primitive.attributes.end() && callbacks.on_access_texcoord) {
        auto& accessor = asset.accessors[attrib->accessorIndex];
        fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, accessor, [&](glm::vec2 uv, usize i) { //
          callbacks.on_access_texcoord(callbacks.user_data, mesh_index, global_vertex_offset + i, uv);
        });
      }

      if (auto attrib = primitive.findAttribute("COLOR");
          attrib != primitive.attributes.end() && callbacks.on_access_color) {
        auto& accessor = asset.accessors[attrib->accessorIndex];
        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, accessor, [&](glm::vec4 color, usize i) { //
          callbacks.on_access_color(callbacks.user_data, mesh_index, global_vertex_offset + i, color);
        });
      }

      global_vertex_offset += primitive_vertex_count;
      global_index_offset += primitive_index_count;
    }
  }

  return model;
}

auto GLTFMeshInfo::parse_info(const ::fs::path& path) -> ox::option<GLTFMeshInfo> {
  ZoneScoped;

  auto gltf_buffer = fastgltf::GltfDataBuffer::FromPath(path);
  if (gltf_buffer.error() != fastgltf::Error::None) {
    OX_LOG_ERROR("Couldn't read gltf data {}", fastgltf::getErrorMessage(gltf_buffer.error()));
    return ox::nullopt;
  }

  fastgltf::Parser parser(get_default_extensions());
  auto result = parser.loadGltf(gltf_buffer.get(), path.parent_path(), get_default_options());
  if (!result) {
    OX_LOG_ERROR("Failed to load GLTF! {}", fastgltf::getErrorMessage(result.error()));
    return ox::nullopt;
  }

  fastgltf::Asset asset = std::move(result.get());
  GLTFMeshInfo model = {};

  ///////////////////////////////////////////////
  // Images
  ///////////////////////////////////////////////

  for (const auto& v : asset.images) {
    auto visitor = fastgltf::visitor{
        [](const auto&) {},
        [&](const fastgltf::sources::ByteView& view) {
          // Embedded buffer
          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<std::vector<u8>>();
          image_info.file_type = to_asset_file_type(view.mimeType);
        },
        [&](const fastgltf::sources::BufferView& view) {
          // Embedded buffer
          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<std::vector<u8>>();
          image_info.file_type = to_asset_file_type(view.mimeType);
        },
        [&](const fastgltf::sources::Array& arr) {
          // Embedded array
          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<std::vector<u8>>();
          image_info.file_type = to_asset_file_type(arr.mimeType);
        },
        [&](const fastgltf::sources::URI& uri) {
          // External file
          const auto& image_file_path = path.parent_path() / uri.uri.fspath();

          auto& image_info = model.images.emplace_back();
          image_info.image_data.emplace<fs::path>(image_file_path);
          image_info.file_type = to_asset_file_type(uri.mimeType);
        },
    };
    std::visit(visitor, v.data);
  }

  ///////////////////////////////////////////////
  // Textures
  ///////////////////////////////////////////////

  for (const auto& v : asset.textures) {
    auto& texture = model.textures.emplace_back();
    if (v.samplerIndex.has_value()) {
      texture.sampler_index = v.samplerIndex.value();
    }

    if (v.imageIndex.has_value()) {
      texture.image_index = v.imageIndex.value();
    } else if (v.basisuImageIndex.has_value()) {
      texture.image_index = v.basisuImageIndex.value();
    }
  }

  ///////////////////////////////////////////////
  // Materials
  ///////////////////////////////////////////////

  for (const auto& v : asset.materials) {
    auto& material = model.materials.emplace_back();
    auto& pbr = v.pbrData;

    material.albedo_color = {
        pbr.baseColorFactor[0],
        pbr.baseColorFactor[1],
        pbr.baseColorFactor[2],
        pbr.baseColorFactor[3],
    };
    material.metallic_factor = pbr.metallicFactor;
    material.roughness_factor = pbr.roughnessFactor;
    material.emissive_color = {
        v.emissiveFactor[0],
        v.emissiveFactor[1],
        v.emissiveFactor[2],
        v.emissiveStrength,
    };
    material.alpha_mode = static_cast<GLTFAlphaMode>(v.alphaMode);
    material.alpha_cutoff = v.alphaCutoff;

    if (auto& tex = pbr.baseColorTexture; tex.has_value()) {
      material.albedo_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.normalTexture; tex.has_value()) {
      material.normal_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.emissiveTexture; tex.has_value()) {
      material.emissive_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.pbrData.metallicRoughnessTexture; tex.has_value()) {
      material.metallic_roughness_texture_index = tex->textureIndex;
    }

    if (auto& tex = v.occlusionTexture; tex.has_value()) {
      material.occlusion_texture_index = tex->textureIndex;
    }
  }

  // We currently need only these, their data doesn't matter
  // for now. Can change it in future.

  return model;
}
} // namespace ox
