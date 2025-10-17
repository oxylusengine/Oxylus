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
  constexpr static int MAX_INT_CVARS = 1000;
  constexpr static int MAX_FLOAT_CVARS = 1000;
  constexpr static int MAX_STRING_CVARS = 200;

  std::vector<CVarStorage<i32>> int_cvars = {};
  std::vector<CVarStorage<f32>> float_cvars = {};
  std::vector<CVarStorage<std::string>> string_cvars = {};

  CVarSystem() = default;

  static CVarSystem* get();
  CVarParameter* get_cvar(usize hash);

  CVarParameter* create_float_cvar(const char* name, const char* description, f32 default_value, f32 current_value);
  CVarParameter* create_int_cvar(const char* name, const char* description, i32 default_value, i32 current_value);
  CVarParameter*
  create_string_cvar(const char* name, const char* description, const char* default_value, const char* current_value);

  f32* get_float_cvar(usize hash);
  i32* get_int_cvar(usize hash);
  std::string* get_string_cvar(usize hash);

  void set_float_cvar(usize hash, f32 value);
  void set_int_cvar(usize hash, i32 value);
  void set_string_cvar(usize hash, const char* value);

private:
  std::shared_mutex mutex_;
  ankerl::unordered_dense::map<usize, std::unique_ptr<CVarParameter>> saved_cvars;
  std::vector<CVarParameter*> cached_edit_parameters;

  CVarParameter* init_cvar(const char* name, const char* description);
};

template <typename T>
struct AutoCVar {
protected:
  u32 index;
  using CVarType = T;
};

struct AutoCVar_Float : AutoCVar<f32> {
  AutoCVar_Float(const char* name, const char* description, f32 default_value, CVarFlags flags = CVarFlags::None);

  auto get() const -> f32;
  auto get_default() const -> f32;
  auto get_ptr() const -> f32*;

  auto set(f32 val) const -> void;
  auto set_default() const -> void;
};

struct AutoCVar_Int : AutoCVar<i32> {
  AutoCVar_Int(const char* name, const char* description, i32 default_value, CVarFlags flags = CVarFlags::None);

  auto get() const -> i32;
  auto get_default() const -> i32;
  auto get_ptr() const -> i32*;

  auto as_bool() const -> bool;

  auto set(i32 val) const -> void;
  auto set_default() const -> void;

  auto toggle() const -> void;
};

struct AutoCVar_String : AutoCVar<std::string> {
  AutoCVar_String(
    const char* name, const char* description, const char* default_value, CVarFlags flags = CVarFlags::None
  );

  auto get() const -> std::string;
  auto set(std::string&& val) const -> void;
};
} // namespace ox
