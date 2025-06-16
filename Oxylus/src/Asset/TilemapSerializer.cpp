#include "TilemapSerializer.hpp"

namespace ox {
void TilemapSerializer::serialize(const std::string& path) {}

void TilemapSerializer::deserialize(const std::string& path) {
  // const auto json = fs::read_file(path);
  // rapidjson::Document doc;
  // rapidjson::ParseResult parse_result = doc.Parse(json.c_str());

  // if (doc.HasParseError()) {
  // OX_LOG_ERROR("Json parser error for: {0} {1}", path, rapidjson::GetParseError_En(parse_result.Code()));
  // return;
  // }

  // TODO:
  // auto identifier = doc["identifier"].GetString();
  // auto uniqueIdentifer = doc["uniqueIdentifer"].GetString();
  // auto x = doc["x"].GetInt();
  // auto y = doc["y"].GetInt();
  // auto bgColor = doc["bgColor"].GetString();
  // auto width = doc["width"].GetInt();
  // auto height = doc["height"].GetInt();

  // _component->tilemap_size = {width, height};
  // TODO: use x, y | bgColor

  // const auto root_path = fs::get_directory(path);
  // for (auto& layer : doc["layers"].GetArray()) {
  // const auto img_path = fs::append_paths(root_path, layer.GetString());
  // auto texture = AssetManager::get_texture_asset({.path = img_path});
  // auto mat = std::make_shared<SpriteMaterial>();
  // mat->set_albedo_texture(texture);
  // _component->layers.emplace(layer.GetString(), mat);
  // }

  // TODO: neighbourLevels, customFields, entities
}
} // namespace ox
