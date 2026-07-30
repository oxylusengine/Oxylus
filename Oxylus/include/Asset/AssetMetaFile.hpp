#pragma once

#include <simdjson.h>

#include "Asset/AssetManager.hpp"

namespace ox {
struct AssetManager::AssetMetaFile {
  simdjson::padded_string contents;
  simdjson::ondemand::parser parser;
  simdjson::simdjson_result<simdjson::ondemand::document> doc;
};
} // namespace ox
