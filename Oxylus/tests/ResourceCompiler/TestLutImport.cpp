#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "AssetImport.hpp"
#include "ResourceCompiler.hpp"
#include "vir/Types.hpp"

using namespace ox;

static auto crc32(std::span<const u8> bytes, u32 crc = 0xFFFFFFFF) -> u32 {
  for (const auto byte : bytes) {
    crc ^= byte;
    for (auto bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
  }

  return crc;
}

static auto push_u32_be(std::vector<u8>& out, u32 value) -> void {
  for (auto shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<u8>(value >> shift));
  }
}

static auto push_chunk(std::vector<u8>& out, std::string_view type, std::span<const u8> data) -> void {
  push_u32_be(out, static_cast<u32>(data.size()));
  auto body = std::vector<u8>(type.begin(), type.end());
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  push_u32_be(out, ~crc32(body));
}

// stored deflate blocks, so no compressor is needed
static auto encode_png(u32 width, u32 height, std::span<const u8> rgba) -> std::vector<u8> {
  auto raw = std::vector<u8>();
  for (auto y = 0_u32; y < height; y++) {
    raw.push_back(0);
    const auto* row = rgba.data() + static_cast<usize>(y) * width * 4;
    raw.insert(raw.end(), row, row + static_cast<usize>(width) * 4);
  }

  auto zlib = std::vector<u8>{0x78, 0x01};
  for (auto pos = 0_sz; pos < raw.size();) {
    const auto len = std::min<usize>(raw.size() - pos, 65535);
    const auto is_last = pos + len == raw.size();
    zlib.push_back(is_last ? 1 : 0);
    zlib.push_back(static_cast<u8>(len));
    zlib.push_back(static_cast<u8>(len >> 8));
    zlib.push_back(static_cast<u8>(~len));
    zlib.push_back(static_cast<u8>(~len >> 8));
    zlib.insert(
      zlib.end(),
      raw.begin() + static_cast<std::ptrdiff_t>(pos),
      raw.begin() + static_cast<std::ptrdiff_t>(pos + len)
    );
    pos += len;
  }

  auto a = 1_u32;
  auto b = 0_u32;
  for (const auto byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }

  push_u32_be(zlib, (b << 16) | a);

  auto png = std::vector<u8>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto header = std::vector<u8>();
  push_u32_be(header, width);
  push_u32_be(header, height);
  header.insert(header.end(), {8, 6, 0, 0, 0});
  push_chunk(png, "IHDR", header);
  push_chunk(png, "IDAT", zlib);
  push_chunk(png, "IEND", {});

  return png;
}

static auto write_bytes(const std::filesystem::path& path, std::span<const u8> bytes) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary)
    .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static auto write_text(const std::filesystem::path& path, std::string_view text) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

static auto neutral_strip_rgba(u32 size) -> std::vector<u8> {
  auto texels = std::vector<u8>(static_cast<usize>(size) * size * size * 4);
  for (auto g = 0_u32; g < size; g++) {
    for (auto b = 0_u32; b < size; b++) {
      for (auto r = 0_u32; r < size; r++) {
        const auto index = (static_cast<usize>(g) * size * size + static_cast<usize>(b) * size + r) * 4;
        texels[index + 0] = static_cast<u8>(r * 255 / (size - 1));
        texels[index + 1] = static_cast<u8>(g * 255 / (size - 1));
        texels[index + 2] = static_cast<u8>(b * 255 / (size - 1));
        texels[index + 3] = 255;
      }
    }
  }

  return texels;
}

static auto neutral_cube(u32 size) -> std::string {
  auto text = std::string("TITLE \"neutral\"\r\n# comment\r\nLUT_3D_SIZE ") + std::to_string(size) +
              "\r\nLUT_3D_INPUT_RANGE 0.0 1.0\r\nDOMAIN_MIN 0.0 0.0 0.0\r\nDOMAIN_MAX 1.0 1.0 1.0\r\n\r\n";
  for (auto b = 0_u32; b < size; b++) {
    for (auto g = 0_u32; g < size; g++) {
      for (auto r = 0_u32; r < size; r++) {
        const auto scale = static_cast<f32>(size - 1);
        text += std::to_string(static_cast<f32>(r) / scale) + " " + std::to_string(static_cast<f32>(g) / scale) + " " +
                std::to_string(static_cast<f32>(b) / scale) + "\r\n";
      }
    }
  }

  return text;
}

struct CookResult {
  bool is_ok = false;
  std::vector<std::string> errors = {};
  std::vector<TextureData> textures = {};
};

static auto cook(const std::filesystem::path& root) -> CookResult {
  auto result = CookResult{};
  auto session = rc::Session::create();
  if (!session.has_value()) {
    return result;
  }

  result.is_ok = rc::cook_assets(*session, root / "Assets", root / "out");
  result.errors = session->take_diagnostics().errors;

  for (const auto& entry : std::filesystem::directory_iterator(root / "out")) {
    if (entry.path().extension() != ".oxpack") {
      continue;
    }

    const auto file = AssetFile::unpack(entry.path());
    if (!file.has_value()) {
      continue;
    }

    for (const auto& packed : file->entries) {
      if (const auto* texture = std::get_if<TextureData>(&packed.data)) {
        result.textures.push_back(*texture);
      }
    }
  }

  session->destroy();

  return result;
}

static auto fresh_root(std::string_view name) -> std::filesystem::path {
  const auto root = std::filesystem::temp_directory_path() / name;
  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root / "out");

  return root;
}

static auto is_failed(const CookResult& result, std::string_view reason) -> bool {
  return result.textures.empty() &&
         std::ranges::any_of(result.errors, [&](const std::string& error) { return error.contains(reason); });
}

TEST(LutImport, NeutralPngKeepsItsTexels) {
  const auto root = fresh_root("ox_test_lut_png");
  const auto texels = neutral_strip_rgba(16);
  write_bytes(root / "Assets" / "neutral_lut_16.png", encode_png(256, 16, texels));

  const auto result = cook(root);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.textures.size(), 1_sz);

  const auto& texture = result.textures[0];
  EXPECT_EQ(texture.vk_format, static_cast<u32>(vir::Format::RGBA8Unorm));
  EXPECT_EQ(texture.width, 256_u32);
  EXPECT_EQ(texture.height, 16_u32);
  ASSERT_EQ(texture.mips.size(), 1_sz);
  EXPECT_EQ(texture.mips[0].pixels, texels);

  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
}

TEST(LutImport, NeutralCubeBecomesHalfFloatStrip) {
  const auto root = fresh_root("ox_test_lut_cube");
  write_text(root / "Assets" / "neutral.cube", neutral_cube(17));

  const auto result = cook(root);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.textures.size(), 1_sz);

  const auto& texture = result.textures[0];
  EXPECT_EQ(texture.vk_format, static_cast<u32>(vir::Format::RGBA16Sfloat));
  EXPECT_EQ(texture.width, 289_u32);
  EXPECT_EQ(texture.height, 17_u32);
  ASSERT_EQ(texture.mips.size(), 1_sz);
  ASSERT_EQ(texture.mips[0].pixels.size(), 289_sz * 17 * 8);

  const auto* halves = reinterpret_cast<const u16*>(texture.mips[0].pixels.data());
  for (auto g = 0_u32; g < 17; g++) {
    for (auto b = 0_u32; b < 17; b++) {
      for (auto r = 0_u32; r < 17; r++) {
        const auto index = (static_cast<usize>(g) * 289 + b * 17 + r) * 4;
        EXPECT_NEAR(glm::unpackHalf1x16(halves[index + 0]), static_cast<f32>(r) / 16.0f, 1e-3f);
        EXPECT_NEAR(glm::unpackHalf1x16(halves[index + 1]), static_cast<f32>(g) / 16.0f, 1e-3f);
        EXPECT_NEAR(glm::unpackHalf1x16(halves[index + 2]), static_cast<f32>(b) / 16.0f, 1e-3f);
        EXPECT_EQ(glm::unpackHalf1x16(halves[index + 3]), 1.0f);
      }
    }
  }

  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
}

TEST(LutImport, PngThatIsNotAStripFails) {
  const auto root = fresh_root("ox_test_lut_bad_png");
  const auto texels = std::vector<u8>(32 * 16 * 4, 255);
  write_bytes(root / "Assets" / "grade.png", encode_png(32, 16, texels));
  write_text(
    root / "Assets" / "grade.png.oxasset",
    R"({"uuid":"0123456789abcdef0123456789abcdef","type":"Texture","usage":"lut"})"
  );

  const auto result = cook(root);
  EXPECT_TRUE(is_failed(result, "16"));

  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
}

TEST(LutImport, OneDimensionalCubeFails) {
  const auto root = fresh_root("ox_test_lut_1d");
  write_text(root / "Assets" / "one.cube", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");

  EXPECT_TRUE(is_failed(cook(root), "1D LUTs are not supported"));

  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
}

TEST(LutImport, CubeWithWideDomainFails) {
  const auto root = fresh_root("ox_test_lut_domain");
  write_text(
    root / "Assets" / "wide.cube",
    "LUT_3D_SIZE 2\nDOMAIN_MAX 2 2 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"
  );

  EXPECT_TRUE(is_failed(cook(root), "only a domain of 0 to 1"));

  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
}

TEST(LutImport, CubeWithMissingRowFails) {
  const auto root = fresh_root("ox_test_lut_short");
  write_text(root / "Assets" / "short.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n");

  EXPECT_TRUE(is_failed(cook(root), "has 7 entries"));

  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
}
