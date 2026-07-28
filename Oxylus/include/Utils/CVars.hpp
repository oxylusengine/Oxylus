#pragma once

#include <ankerl/unordered_dense.h>
#include <shared_mutex>
#include <vector>

#include "Core/Types.hpp"

namespace ox {
enum class CVarFlags : u32 {
  None = 0,
  Noedit = 1 << 1,
  EditReadOnly = 1 << 2,
  Advanced = 1 << 3,
  Dropdown = 1 << 4,

  EditCheckbox = 1 << 8,
  EditFloatDrag = 1 << 9,
  EditIntDrag = 1 << 10,
};

enum class CVarType : char {
  INT,
  FLOAT,
  STRING,
};

class CVarParameter {
public:
  u32 array_index;

  CVarType type;
  CVarFlags flags;
  std::string name;
  std::string description;
};

template <typename T>
struct CVarStorage {
  T initial;
  T current;
  CVarParameter* parameter;
};

class CVarSystem {
public:
  std::vector<CVarStorage<i32>> int_cvars = {};
  std::vector<CVarStorage<f32>> float_cvars = {};
  std::vector<CVarStorage<std::string>> string_cvars = {};

  CVarSystem() = default;

  auto get_cvar(usize hash) -> CVarParameter*;

  auto create_float_cvar(std::string_view name, std::string_view description, f32 default_value, f32 current_value)
    -> CVarParameter*;
  auto create_int_cvar(std::string_view name, std::string_view description, i32 default_value, i32 current_value)
    -> CVarParameter*;
  auto create_string_cvar(
    std::string_view name, std::string_view description, std::string_view default_value, std::string_view current_value
  ) -> CVarParameter*;

  auto get_float_cvar(usize hash) -> f32*;
  auto get_int_cvar(usize hash) -> i32*;
  auto get_string_cvar(usize hash) -> std::string*;

  auto set_float_cvar(usize hash, f32 value) -> void;
  auto set_int_cvar(usize hash, i32 value) -> void;
  auto set_string_cvar(usize hash, std::string_view value) -> void;

private:
  std::shared_mutex mutex_;
  ankerl::unordered_dense::map<usize, std::unique_ptr<CVarParameter>> saved_cvars;
  std::vector<CVarParameter*> cached_edit_parameters;

  auto init_cvar(std::string_view name, std::string_view description) -> CVarParameter*;
};

class CVarInterface {
public:
  CVarSystem system = {};
};

template <typename T>
struct AutoCVar {
protected:
  CVarSystem* system = nullptr;
  u32 index;
  using CVarType = T;
};

struct AutoCVar_Float : AutoCVar<f32> {
  AutoCVar_Float() = default;
  auto init(
    CVarSystem& cvar_system,
    const char* name,
    const char* description,
    const f32 default_value,
    const CVarFlags flags = CVarFlags::None
  ) -> void;

  auto get() const -> f32;
  auto get_default() const -> f32;
  auto get_ptr() const -> f32*;

  auto set(f32 val) const -> void;
  auto set_default() const -> void;
};

struct AutoCVar_Int : AutoCVar<i32> {
  AutoCVar_Int() = default;
  auto init(
    CVarSystem& cvar_system,
    const char* name,
    const char* description,
    i32 default_value,
    CVarFlags flags = CVarFlags::None
  ) -> void;

  auto get() const -> i32;
  auto get_default() const -> i32;
  auto get_ptr() const -> i32*;

  auto as_bool() const -> bool;

  auto set(i32 val) const -> void;
  auto set_default() const -> void;

  auto toggle() const -> void;
};

struct AutoCVar_String : AutoCVar<std::string> {
  auto init(
    CVarSystem& cvar_system,
    const char* name,
    const char* description,
    const char* default_value,
    CVarFlags flags = CVarFlags::None
  ) -> void;

  auto get() const -> std::string;
  auto set(std::string&& val) const -> void;
};
} // namespace ox
