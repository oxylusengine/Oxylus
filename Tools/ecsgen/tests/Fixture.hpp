#pragma once

#include "Scene/ComponentReflection.hpp"

#define FIXTURE_MULTILINE_MACRO(x) \
  struct x {                       \
    int broken(;                   \
  };

namespace ox {
struct NotAComponent {
  int ignored(;
};

OX_COMPONENT(networked, name = "Renamed")
struct PlainComponent {
  f32 a = 1.0f, b = 2.0f;
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  std::array<f32, 4> weights = {};
  glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
  OX_TRANSIENT void* runtime = nullptr;
  OX_FIELD(transient) u32 counter = 0;

  static constexpr u32 LIMIT = 4;
  using Alias = u32;

  auto sum(this const PlainComponent& self) -> f32 { return self.a + self.b; }
  auto defaulted() -> void = delete;
};

OX_COMPONENT()
struct EnumComponent {
  enum Mode : u32 {
    First = 0b01, ///< first, with 'quotes'
    Second = 0b10, /* block */
  };
  enum class Unused { A, B };
  OX_ENUM() enum class Forced : u8 { X };
  OX_ENUM("Kind") enum Kind { K0 };

  struct Nested {
    int x = 0;
  };

  Mode mode = Mode::First;
  EnumComponent::Kind kind = K0;
  OX_TRANSIENT Nested nested = {};
  OX_FIELD(asset = Texture) UUID texture = {};

  template <typename Self>
  auto pick(this Self& self, const u32 index) -> auto& {
    switch (index) {
      default: return self.mode;
    }
  }
};
} // namespace ox
