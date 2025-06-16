#pragma once

namespace ox {
class EmbedAsset {
public:
  static void EmbedTexture(const std::string& texFilePath, const std::string& outPath, const std::string& arrayName);
};
} // namespace ox
