#pragma once

namespace ox {
struct TilemapComponent;
class TilemapSerializer {
public:
  TilemapSerializer(TilemapComponent* component) : _component(component) {}

  void serialize(const std::string& path);
  void deserialize(const std::string& path);

private:
  TilemapComponent* _component = nullptr;
};
} // namespace ox
