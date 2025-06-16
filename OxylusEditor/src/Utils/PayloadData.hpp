#pragma once

#include <imgui.h>

#include "Core/UUID.hpp"

namespace ox {
struct PayloadData {
  static constexpr auto DRAG_DROP_TARGET = "CONTENT_BROWSER_ITEM_TARGET";
  static constexpr auto DRAG_DROP_SOURCE = "CONTENT_BROWSER_ITEM_SOURCE";

  char str[256] = {};
  UUID uuid = {};

  PayloadData(const std::string& s, const UUID& id = {}) {
    OX_CHECK_LT(s.size(), sizeof(str), "String can't fit into payload");

    std::strncpy(str, s.c_str(), sizeof(str));
    str[sizeof(str) - 1] = '\0'; // null-termination
    uuid = id;
  }

  auto size() const -> usize { return sizeof(PayloadData); }

  auto get_str() const -> std::string { return std::string(str); }

  static auto from_payload(const ImGuiPayload* payload) -> const PayloadData* {
    return reinterpret_cast<const PayloadData*>(payload->Data);
  }
};

} // namespace ox
