#include "Animation/Skeleton.hpp"

#include "Memory/Hasher.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto Skeleton::find_bone(this const Skeleton& self, const std::string_view name) -> option<u32> {
  const auto it = self.bone_index_lut.find(fnv64_str(name));
  if (it == self.bone_index_lut.end()) {
    return nullopt;
  }

  return it->second;
}

auto Skeleton::finalize(this Skeleton& self) -> bool {
  ZoneScoped;

  const auto count = self.bone_count();
  if (
    count == 0 || self.bone_names.size() != count || self.parent_space_reference_pose.size() != count ||
    self.inverse_bind_pose.size() != count
  ) {
    return false;
  }

  self.bone_name_hashes.resize(count);
  self.bone_index_lut.clear();
  self.bone_index_lut.reserve(count);
  for (auto i = 0_u32; i < count; ++i) {
    const auto hash = fnv64_str(self.bone_names[i]);
    self.bone_name_hashes[i] = hash;
    self.bone_index_lut.emplace(hash, i);
  }

  self.model_space_reference_pose.resize(count);
  for (auto i = 0_u32; i < count; ++i) {
    const auto parent = self.parent_indices[i];
    if (parent >= static_cast<i32>(i)) {
      OX_LOG_ERROR("Skeleton bone '{}' has parent {} which is not ordered before it.", self.bone_names[i], parent);
      return false;
    }

    self.model_space_reference_pose[i] = parent < 0 ? self.parent_space_reference_pose[i]
                                                    : self.model_space_reference_pose[parent] *
                                                        self.parent_space_reference_pose[i];
  }

  return true;
}

auto Skeleton::is_valid(this const Skeleton& self) -> bool {
  const auto count = self.bone_count();
  return count > 0 && self.model_space_reference_pose.size() == count && self.inverse_bind_pose.size() == count;
}
} // namespace ox
