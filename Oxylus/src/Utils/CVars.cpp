#include "Utils/CVars.hpp"

namespace ox {
auto CVarSystem::get_float_cvar(const usize hash) -> f32* {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &float_cvars[par->array_index].current;
}

auto CVarSystem::get_int_cvar(const usize hash) -> i32* {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &int_cvars[par->array_index].current;
}

auto CVarSystem::get_string_cvar(const usize hash) -> std::string* {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &string_cvars[par->array_index].current;
}

auto CVarSystem::get_cvar(const usize hash) -> CVarParameter* {
  std::shared_lock lock(mutex_);

  if (saved_cvars.contains(hash))
    return saved_cvars[hash].get();

  return nullptr;
}

auto CVarSystem::set_float_cvar(const usize hash, const f32 value) -> void {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    float_cvars[cvar->array_index].current = value;
}

auto CVarSystem::set_int_cvar(const usize hash, const i32 value) -> void {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    int_cvars[cvar->array_index].current = value;
}

auto CVarSystem::set_string_cvar(const usize hash, std::string_view value) -> void {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    string_cvars[cvar->array_index].current = value;
}

auto CVarSystem::create_float_cvar(
  std::string_view name, std::string_view description, const f32 default_value, const f32 current_value
) -> CVarParameter* {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::FLOAT;
  param->array_index = (u32)float_cvars.size();
  float_cvars.emplace_back(default_value, current_value, param);

  return param;
}

auto CVarSystem::create_int_cvar(
  std::string_view name, std::string_view description, const i32 default_value, const i32 current_value
) -> CVarParameter* {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::INT;
  param->array_index = (u32)int_cvars.size();
  int_cvars.emplace_back(default_value, current_value, param);

  return param;
}

auto CVarSystem::create_string_cvar(
  std::string_view name, std::string_view description, std::string_view default_value, std::string_view current_value
) -> CVarParameter* {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::STRING;
  param->array_index = (u32)string_cvars.size();
  string_cvars.emplace_back(default_value.data(), current_value.data(), param);

  return param;
}

auto CVarSystem::init_cvar(std::string_view name, std::string_view description) -> CVarParameter* {
  const std::hash<std::string> hasher = {};
  const auto name_str = std::string(name);
  const usize namehash = hasher(name_str);
  return saved_cvars
    .emplace(namehash, std::make_unique<CVarParameter>(CVarParameter({}, {}, {}, name.data(), description.data())))
    .first->second.get();
}

auto AutoCVar_Float::init(
  CVarSystem& cvar_system, const char* name, const char* description, const f32 default_value, const CVarFlags flags
) -> void {
  system = &cvar_system;
  CVarParameter* cvar = system->create_float_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}
auto AutoCVar_Float::get() const -> f32 { return system->float_cvars.at(index).current; }
auto AutoCVar_Float::get_default() const -> f32 { return system->float_cvars.at(index).initial; }
auto AutoCVar_Float::get_ptr() const -> f32* { return &system->float_cvars[index].current; }
auto AutoCVar_Float::set(const f32 val) const -> void { system->float_cvars.at(index).current = val; }
auto AutoCVar_Float::set_default() const -> void {
  auto& c = system->float_cvars.at(index);
  c.current = c.initial;
}

auto AutoCVar_Int::init(
  CVarSystem& cvar_system, const char* name, const char* description, i32 default_value, CVarFlags flags
) -> void {
  system = &cvar_system;
  CVarParameter* cvar = cvar_system.create_int_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}
auto AutoCVar_Int::get() const -> i32 { return system->int_cvars.at(index).current; }
auto AutoCVar_Int::get_default() const -> i32 { return system->int_cvars.at(index).initial; }
auto AutoCVar_Int::get_ptr() const -> i32* { return &system->int_cvars.at(index).current; }
auto AutoCVar_Int::as_bool() const -> bool { return static_cast<bool>(get()); }
auto AutoCVar_Int::set(const i32 val) const -> void { system->int_cvars.at(index).current = val; }
auto AutoCVar_Int::set_default() const -> void {
  auto& c = system->int_cvars.at(index);
  c.current = c.initial;
}
auto AutoCVar_Int::toggle() const -> void {
  const bool enabled = get() != 0;

  set(enabled ? 0 : 1);
}

auto AutoCVar_String::init(
  CVarSystem& cvar_system, const char* name, const char* description, const char* default_value, const CVarFlags flags
) -> void {
  system = &cvar_system;
  CVarParameter* cvar = system->create_string_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}
auto AutoCVar_String::get() const -> std::string { return system->string_cvars.at(index).current; };
auto AutoCVar_String::set(std::string&& val) const -> void { system->string_cvars.at(index).current = val; }
} // namespace ox
