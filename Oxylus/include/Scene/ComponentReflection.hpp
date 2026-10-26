#pragma once

#include <span>

#include "Core/Types.hpp"

// markers read by ecsgen (Tools/ecsgen), they expand to nothing
#define OX_COMPONENT(...)
#define OX_FIELD(...)
#define OX_TRANSIENT
#define OX_ENUM(...)

namespace ox {
enum class AssetType : u32;

struct AssetField {
  const c8* member;
  AssetType type;
};

// set on a component entity from its OX_FIELD(asset = ...) fields, tells the inspector which asset each UUID
// field points at. flecs only creates member entities with FLECS_CREATE_MEMBER_ENTITIES, so it can't live on those
struct AssetFields {
  std::span<const AssetField> fields;
};
} // namespace ox
