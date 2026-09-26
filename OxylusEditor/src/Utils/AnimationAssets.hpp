#pragma once

#include <ankerl/svector.h>

#include "Core/UUID.hpp"

namespace ox {
// a skeleton or clip carries the path of the model it was imported out of, so the registry can
// bridge between them without reopening the `.oxasset` sidecar
auto find_source_model(const UUID& asset_uuid) -> UUID;
// import order, and empty when the model itself is not loaded
auto model_animation_clips(const UUID& model_uuid) -> ankerl::svector<UUID, 8>;
// every clip sharing an asset's file, for when the model is unavailable
auto sibling_animation_clips(const UUID& asset_uuid) -> ankerl::svector<UUID, 8>;
} // namespace ox
