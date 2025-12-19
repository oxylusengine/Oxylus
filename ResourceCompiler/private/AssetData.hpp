#pragma once

#include <Asset/AssetFile.hpp>
#include <Core/Types.hpp>
#include <vector>

namespace ox::rc {
constexpr auto push_str(std::vector<u8>& asset_data, std::string_view str) -> AssetDataView<c8> {
  auto begin = asset_data.size();
  asset_data.insert(asset_data.end(), str.begin(), str.end());
  auto end = asset_data.size();
  asset_data.insert(asset_data.end(), '\0');

  return AssetDataView<c8>(begin, end);
}

template <typename T>
constexpr auto push_span(std::vector<u8>& asset_data, std::span<const T> span) -> AssetDataView<T> {
  auto begin = asset_data.size();
  const auto* bytes = reinterpret_cast<const u8*>(span.data());
  asset_data.insert(asset_data.end(), bytes, bytes + ox::size_bytes(span));
  auto end = asset_data.size();

  return AssetDataView<T>(begin, end);
}

template <typename T>
  requires std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> && (not std::is_class_v<T>)
constexpr auto push_trivial(std::vector<u8>& asset_data, T& v) -> AssetDataView<T> {
  auto begin = asset_data.size();
  asset_data.insert(asset_data.end(), v);
  auto end = asset_data.size();

  return AssetDataView<T>(begin, end);
}

} // namespace ox::rc
