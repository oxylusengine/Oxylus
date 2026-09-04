#include "Asset/AssetFile.hpp"

#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

#include "Animation/AnimationClip.hpp"
#include "Animation/Skeleton.hpp"
#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
// caps any single length-prefixed container in a pack; the largest thing we store is a mesh blob
constexpr static auto MAX_ENTRY_ELEMENTS = 1_u64 << 31;

auto PackedUUID::pack(const UUID& uuid) -> PackedUUID {
  auto self = PackedUUID{};
  std::ranges::copy(uuid.bytes(), self.bytes.begin());
  return self;
}

auto PackedUUID::unpack(this const PackedUUID& self) -> UUID {
  auto bytes = self.bytes;
  return UUID::from_bytes(bytes).value_or(UUID(nullptr));
}

auto to_material(const ModelData::Material& src, std::span<const UUID> textures) -> Material {
  const auto resolve = [&](u32 index) -> UUID {
    return index < textures.size() ? textures[index] : UUID(nullptr);
  };

  return Material{
    .albedo_color = glm::make_vec4(src.albedo_color.data()),
    .uv_size = glm::make_vec2(src.uv_size.data()),
    .uv_offset = glm::make_vec2(src.uv_offset.data()),
    .emissive_color = glm::make_vec3(src.emissive_color.data()),
    .roughness_factor = src.roughness_factor,
    .metallic_factor = src.metallic_factor,
    .normal_scale = src.normal_scale,
    .occlusion_strength = src.occlusion_strength,
    .alpha_mode = src.alpha_mode,
    .alpha_cutoff = src.alpha_cutoff,
    .sampling_mode = src.sampling_mode,
    .flip_normal_y = src.flip_normal_y,
    .albedo_texture = resolve(src.albedo_texture_index),
    .normal_texture = resolve(src.normal_texture_index),
    .emissive_texture = resolve(src.emissive_texture_index),
    .metallic_roughness_texture = resolve(src.metallic_roughness_texture_index),
    .occlusion_texture = resolve(src.occlusion_texture_index),
  };
}

auto to_bone_transform(const ModelData::BoneTransform& src) -> BoneTransform {
  return BoneTransform{
    .rotation = glm::quat::wxyz(src.rotation[3], src.rotation[0], src.rotation[1], src.rotation[2]),
    .translation_scale = glm::make_vec4(src.translation_scale.data()),
  };
}

auto to_skeleton(const ModelData::Skeleton& src) -> Skeleton {
  ZoneScoped;

  auto skeleton = Skeleton{};
  skeleton.bone_names = src.bone_names;
  skeleton.parent_indices = src.parent_indices;

  skeleton.parent_space_reference_pose.reserve(src.parent_space_reference_pose.size());
  for (const auto& bone : src.parent_space_reference_pose) {
    skeleton.parent_space_reference_pose.push_back(to_bone_transform(bone));
  }

  skeleton.inverse_bind_pose.reserve(src.inverse_bind_pose.size());
  for (const auto& bone : src.inverse_bind_pose) {
    skeleton.inverse_bind_pose.push_back(to_bone_transform(bone));
  }

  if (!skeleton.finalize()) {
    return {};
  }

  return skeleton;
}

auto to_animation_clip(const ModelData::Animation& src, const UUID& skeleton_uuid) -> AnimationClip {
  ZoneScoped;

  auto clip = AnimationClip{};
  clip.name = src.name;
  clip.skeleton_uuid = skeleton_uuid;
  clip.frame_count = src.frame_count;
  clip.duration = src.duration;
  clip.compressed_pose_data = src.compressed_pose_data;
  clip.compressed_pose_offsets = src.compressed_pose_offsets;

  clip.track_defs.reserve(src.track_defs.size());
  for (const auto& track : src.track_defs) {
    clip.track_defs.push_back(
      TrackDefinition{
        .translation_range_x = {track.translation_range_x[0], track.translation_range_x[1]},
        .translation_range_y = {track.translation_range_y[0], track.translation_range_y[1]},
        .translation_range_z = {track.translation_range_z[0], track.translation_range_z[1]},
        .scale_range = {track.scale_range[0], track.scale_range[1]},
        .constant_rotation = {track.constant_rotation[0], track.constant_rotation[1], track.constant_rotation[2]},
        .track_read_offset = track.track_read_offset,
        .is_rotation_static = track.is_rotation_static,
        .is_translation_static = track.is_translation_static,
        .is_scale_static = track.is_scale_static,
      }
    );
  }

  return clip;
}

auto to_model_bone_transform(const BoneTransform& src) -> ModelData::BoneTransform {
  return ModelData::BoneTransform{
    .rotation = {src.rotation.x, src.rotation.y, src.rotation.z, src.rotation.w},
    .translation_scale =
      {src.translation_scale.x, src.translation_scale.y, src.translation_scale.z, src.translation_scale.w},
  };
}

auto to_model_skeleton(const Skeleton& src) -> ModelData::Skeleton {
  ZoneScoped;

  auto skeleton = ModelData::Skeleton{};
  skeleton.bone_names = src.bone_names;
  skeleton.parent_indices = src.parent_indices;

  skeleton.parent_space_reference_pose.reserve(src.parent_space_reference_pose.size());
  for (const auto& bone : src.parent_space_reference_pose) {
    skeleton.parent_space_reference_pose.push_back(to_model_bone_transform(bone));
  }

  skeleton.inverse_bind_pose.reserve(src.inverse_bind_pose.size());
  for (const auto& bone : src.inverse_bind_pose) {
    skeleton.inverse_bind_pose.push_back(to_model_bone_transform(bone));
  }

  return skeleton;
}

auto to_model_animation(const AnimationClip& src) -> ModelData::Animation {
  ZoneScoped;

  auto animation = ModelData::Animation{};
  animation.name = src.name;
  animation.frame_count = src.frame_count;
  animation.duration = src.duration;
  animation.compressed_pose_data = src.compressed_pose_data;
  animation.compressed_pose_offsets = src.compressed_pose_offsets;

  animation.track_defs.reserve(src.track_defs.size());
  for (const auto& track : src.track_defs) {
    animation.track_defs.push_back(
      ModelData::AnimationTrack{
        .translation_range_x = {track.translation_range_x.start, track.translation_range_x.length},
        .translation_range_y = {track.translation_range_y.start, track.translation_range_y.length},
        .translation_range_z = {track.translation_range_z.start, track.translation_range_z.length},
        .scale_range = {track.scale_range.start, track.scale_range.length},
        .constant_rotation =
          {track.constant_rotation.data0, track.constant_rotation.data1, track.constant_rotation.data2},
        .track_read_offset = track.track_read_offset,
        .is_rotation_static = track.is_rotation_static,
        .is_translation_static = track.is_translation_static,
        .is_scale_static = track.is_scale_static,
      }
    );
  }

  return animation;
}

auto AssetFile::unpack(const std::filesystem::path& path) -> option<AssetFile> {
  ZoneScoped;

  auto file = File(path, FileAccess::Read);
  auto* mapped_data = file.map();
  auto bytes = std::span(static_cast<u8*>(mapped_data), file.size);
  // a pack is untrusted input: a corrupt length prefix must not turn into a huge allocation
  auto deser = zpp::bits::in(bytes, zpp::bits::alloc_limit<MAX_ENTRY_ELEMENTS>{});

  auto header = AssetFileHeader{};
  auto entries = std::vector<AssetFileEntry>();

  if (zpp::bits::failure(deser(header))) {
    OX_LOG_ERROR("Failed to deserialize Asset Header.");
    return nullopt;
  }

  if (header.magic != AssetFileHeader::SIGNATURE) {
    OX_LOG_ERROR("Failed to deserialize Asset Header. Signatures don't match.");
    return nullopt;
  }

  if (header.version != AssetFileHeader::VERSION) {
    OX_LOG_ERROR(
      "Asset file '{}' was built with version {}, expected {}. Recompile it.",
      path,
      header.version,
      AssetFileHeader::VERSION
    );
    return nullopt;
  }

  if (zpp::bits::failure(deser(entries))) {
    OX_LOG_ERROR("Failed to deserialize Asset entries.");
    return nullopt;
  }

  return AssetFile{
    .flags = header.flags,
    .entries = std::move(entries),
  };
}

auto AssetFile::pack(this AssetFile& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto header = AssetFileHeader{
    .flags = self.flags,
  };

  auto [data, ser] = zpp::bits::data_out();
  if (zpp::bits::failure(ser(header, self.entries))) {
    OX_LOG_ERROR("Failed to serialize asset file.");
    return false;
  }

  auto file = File(path, FileAccess::Write);
  if (!file) {
    OX_LOG_ERROR("Failed to open '{}' for writing.", path);
    return false;
  }

  file.write(data);

  return true;
}

auto AssetFile::add_entry(this AssetFile& self, ShaderPipelineData&& entry, const PackedUUID& uuid) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .uuid = uuid,
      .type = AssetType::Shader,
      .data = std::move(entry),
    }
  );
}

auto AssetFile::add_entry(this AssetFile& self, TextureData&& entry, const PackedUUID& uuid) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .uuid = uuid,
      .type = AssetType::Texture,
      .data = std::move(entry),
    }
  );
}

auto AssetFile::add_entry(this AssetFile& self, ModelData&& entry, const PackedUUID& uuid) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .uuid = uuid,
      .type = AssetType::Model,
      .data = std::move(entry),
    }
  );
}

} // namespace ox
