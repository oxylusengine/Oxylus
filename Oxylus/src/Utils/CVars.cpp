#include "Utils/CVars.hpp"

namespace ox {
f32* CVarSystem::get_float_cvar(const usize hash) {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &float_cvars[par->array_index].current;
}

i32* CVarSystem::get_int_cvar(const usize hash) {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &int_cvars[par->array_index].current;
}

std::string* CVarSystem::get_string_cvar(const usize hash) {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &string_cvars[par->array_index].current;
}

CVarSystem* CVarSystem::get() {
  static CVarSystem cvar_sys{};
  return &cvar_sys;
}

CVarParameter* CVarSystem::get_cvar(const usize hash) {
  std::shared_lock lock(mutex_);

  if (saved_cvars.contains(hash))
    return saved_cvars[hash].get();

  return nullptr;
}

void CVarSystem::set_float_cvar(const usize hash, const f32 value) {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    float_cvars[cvar->array_index].current = value;
}

void CVarSystem::set_int_cvar(const usize hash, const i32 value) {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    int_cvars[cvar->array_index].current = value;
}

void CVarSystem::set_string_cvar(const usize hash, const char* value) {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    string_cvars[cvar->array_index].current = value;
}

CVarParameter* CVarSystem::create_float_cvar(
  const char* name, const char* description, const f32 default_value, const f32 current_value
) {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::FLOAT;
  param->array_index = (u32)float_cvars.size();
  float_cvars.emplace_back(default_value, current_value, param);

  return param;
}

CVarParameter* CVarSystem::create_int_cvar(
  const char* name, const char* description, const i32 default_value, const i32 current_value
) {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::INT;
  param->array_index = (u32)int_cvars.size();
  int_cvars.emplace_back(default_value, current_value, param);

  return param;
}

CVarParameter* CVarSystem::create_string_cvar(
  const char* name, const char* description, const char* default_value, const char* current_value
) {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::STRING;
  param->array_index = (u32)string_cvars.size();
  string_cvars.emplace_back(default_value, current_value, param);

  return param;
}

CVarParameter* CVarSystem::init_cvar(const char* name, const char* description) {
  const std::hash<std::string> hasher = {};
  const auto name_str = std::string(name);
  const usize namehash = hasher(name_str);
  return saved_cvars.emplace(namehash, std::make_unique<CVarParameter>(CVarParameter({}, {}, {}, name, description)))
    .first->second.get();
}

AutoCVar_Float::AutoCVar_Float(
  const char* name, const char* description, const f32 default_value, const CVarFlags flags
) {
  CVarParameter* cvar = CVarSystem::get()->create_float_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}

auto AutoCVar_Float::get() const -> f32 { return CVarSystem::get()->float_cvars.at(index).current; }
auto AutoCVar_Float::get_default() const -> f32 { return CVarSystem::get()->int_cvars.at(index).initial; }
auto AutoCVar_Float::get_ptr() const -> f32* { return &CVarSystem::get()->float_cvars[index].current; }

auto AutoCVar_Float::set(const f32 val) const -> void { CVarSystem::get()->float_cvars.at(index).current = val; }
auto AutoCVar_Float::set_default() const -> void {
  auto& c = CVarSystem::get()->float_cvars.at(index);
  c.current = c.initial;
}

AutoCVar_Int::AutoCVar_Int(const char* name, const char* description, const i32 default_value, const CVarFlags flags) {
  CVarParameter* cvar = CVarSystem::get()->create_int_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}

auto AutoCVar_Int::get() const -> i32 { return CVarSystem::get()->int_cvars.at(index).current; }
auto AutoCVar_Int::get_default() const -> i32 { return CVarSystem::get()->int_cvars.at(index).initial; }
auto AutoCVar_Int::get_ptr() const -> i32* { return &CVarSystem::get()->int_cvars.at(index).current; }

auto AutoCVar_Int::as_bool() const -> bool { return static_cast<bool>(get()); }

auto AutoCVar_Int::set(const i32 val) const -> void { CVarSystem::get()->int_cvars.at(index).current = val; }
auto AutoCVar_Int::set_default() const -> void {
  auto& c = CVarSystem::get()->int_cvars.at(index);
  c.current = c.initial;
}

auto AutoCVar_Int::toggle() const -> void {
  const bool enabled = get() != 0;

  set(enabled ? 0 : 1);
}

AutoCVar_String::AutoCVar_String(
  const char* name, const char* description, const char* default_value, const CVarFlags flags
) {
  CVarParameter* cvar = CVarSystem::get()->create_string_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}

auto AutoCVar_String::get() const -> std::string { return CVarSystem::get()->string_cvars.at(index).current; };

auto AutoCVar_String::set(std::string&& val) const -> void { CVarSystem::get()->string_cvars.at(index).current = val; }
} // namespace ox
