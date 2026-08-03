#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <flecs.h>
#include <string_view>
#include <type_traits>

#ifdef OX_LUA_BINDINGS
  #include <sol/sol.hpp>
#endif

#include "Core/Types.hpp"

namespace ox {
// Compile time reflection over pointers to members. Component and member names are derived from the
// pointer itself, so a registration can't disagree with the struct it describes.
namespace refl {
template <typename T>
struct member_traits;

template <typename C, typename M>
struct member_traits<M C::*> {
  using owner = C;
  using type = M;
};

template <auto P>
using member_owner_t = typename member_traits<decltype(P)>::owner;

template <auto P>
using member_type_t = typename member_traits<decltype(P)>::type;

template <auto First, auto...>
struct first_of {
  static constexpr auto value = First;
};

// Owner of a pack of pointers to members, i.e. the component being described.
template <auto... Members>
using owner_t = member_owner_t<first_of<Members...>::value>;

// Pulls the member name out of the compiler's signature for this function:
//   clang: "auto ox::refl::member_name() [P = &ox::TransformComponent::position]"
//   gcc:   "consteval auto ox::refl::member_name() [with auto P = &ox::TransformComponent::position]"
//   msvc:  "auto __cdecl ox::refl::member_name<&ox::TransformComponent::position>(void)"
// In every form the member is the last "::" separated identifier, so scan back to it and take the
// identifier that follows.
template <auto P>
consteval auto member_name() -> std::string_view {
  constexpr auto sig = std::string_view{ECS_FUNC_NAME};
  constexpr auto scope = sig.rfind("::");
  static_assert(scope != std::string_view::npos, "unrecognized compiler signature format");

  constexpr auto is_identifier = [](const c8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };

  const auto begin = scope + 2;
  auto end = begin;
  while (end < sig.size() && is_identifier(sig[end]))
    end += 1;

  return sig.substr(begin, end - begin);
}

// flecs wants a null terminated name, which a view into the signature isn't.
template <auto P>
inline constexpr auto member_name_buffer = [] {
  constexpr auto name = member_name<P>();
  auto buffer = std::array<c8, name.size() + 1>{};
  std::ranges::copy(name, buffer.begin());
  return buffer;
}();

template <auto P>
consteval auto member_cstr() -> const c8* {
  return member_name_buffer<P>.data();
}
} // namespace refl

// "ox::TransformComponent" -> "TransformComponent". Components are registered inside a module scope,
// so the C++ namespace would only add noise to the entity path.
template <typename T>
auto short_type_name() -> const c8* {
  const auto* full_name = flecs::_::type_name<T>();
  const auto* last_scope = std::strrchr(full_name, ':');
  return last_scope != nullptr ? last_scope + 1 : full_name;
}

struct ComponentBuilder {
  flecs::untyped_component component;

  template <typename... Tags>
  auto tags(this ComponentBuilder self) -> ComponentBuilder {
    (self.component.add<Tags>(), ...);
    return self;
  }

  auto add(this ComponentBuilder self, flecs::entity e) -> ComponentBuilder {
    self.component.add(e);
    return self;
  }

  operator flecs::entity() const { return component; }
};

// Registers types with flecs and, when scripting is enabled, mirrors components into a Lua module table.
struct ComponentRegistry {
  flecs::world& world;

#ifdef OX_LUA_BINDINGS
  sol::state* state = nullptr;
  sol::table module_table = {};

  ComponentRegistry(flecs::world& world_, sol::state* state_, sol::table module_table_)
      : world(world_),
        state(state_),
        module_table(module_table_) {}
#else
  explicit ComponentRegistry(flecs::world& world_) : world(world_) {}
#endif

  // Registers a component together with its members. The component type is deduced from the member
  // pointers, and both it and the member names are derived from the pointers.
  template <auto... Members>
  auto bind(this ComponentRegistry& self, const c8* name = nullptr) -> ComponentBuilder {
    using T = refl::owner_t<Members...>;
    static_assert(
      (std::is_same_v<T, refl::member_owner_t<Members>> && ...),
      "all members must belong to the same component"
    );

    auto component = self.world.template component<T>(name != nullptr ? name : short_type_name<T>());
    (component.member(refl::member_cstr<Members>(), Members), ...);

#ifdef OX_LUA_BINDINGS
    if (self.state != nullptr) {
      auto usertype = self.state->create_named_table(component.name().c_str());
      usertype["component_id"] = static_cast<u64>(component.id());
      ((usertype[refl::member_cstr<Members>()] = Members), ...);
      self.module_table[component.name().c_str()] = usertype;
    }
#endif

    return ComponentBuilder{component};
  }

  // Value types (glm and friends) need flecs meta for serialization, but no Lua module entry. Their
  // real type names are template ids, so they're always named explicitly.
  template <auto... Members>
  auto bind_value(this ComponentRegistry& self, const c8* name) -> flecs::untyped_component {
    using T = refl::owner_t<Members...>;
    static_assert(
      (std::is_same_v<T, refl::member_owner_t<Members>> && ...),
      "all members must belong to the same type"
    );

    auto component = self.world.template component<T>(name);
    (component.member(refl::member_cstr<Members>(), Members), ...);
    return component;
  }

  // Column major matrices, whose columns aren't addressable as named members.
  template <typename T, typename Column, usize ColumnCount>
  auto bind_matrix(this ComponentRegistry& self, const c8* name) -> flecs::untyped_component {
    constexpr auto column_names = std::array<const c8*, 4>{"col0", "col1", "col2", "col3"};
    static_assert(ColumnCount <= column_names.size(), "unsupported column count");

    auto component = self.world.template component<T>(name);
    for (usize i = 0; i < ColumnCount; i++)
      component.template member<Column>(column_names[i]);

    return component;
  }

  // Registering the enum itself makes flecs pick up its constants, so members typed with it serialize
  // and display as names instead of raw integers. Has to happen before any component referencing it is
  // bound, otherwise flecs registers the enum implicitly under a name derived from the C++ path.
  template <typename E>
  auto bind_enum(this ComponentRegistry& self, const c8* name = nullptr) -> flecs::untyped_component {
    static_assert(std::is_enum_v<E>, "bind_enum requires an enum type");
    return self.world.template component<E>(name != nullptr ? name : short_type_name<E>());
  }
};
} // namespace ox
