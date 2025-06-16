#include "CVars.hpp"

namespace ox {
float* CVarSystem::get_float_cvar(const usize hash) {
  ZoneScoped;
  const CVarParameter* par = get_cvar(hash);
  if (!par) {
    return nullptr;
  }
  return &float_cvars[par->array_index].current;
}

int32_t* CVarSystem::get_int_cvar(const usize hash) {
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

void CVarSystem::set_float_cvar(const usize hash, const float value) {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    float_cvars[cvar->array_index].current = value;
}

void CVarSystem::set_int_cvar(const usize hash, const int32_t value) {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    int_cvars[cvar->array_index].current = value;
}

void CVarSystem::set_string_cvar(const usize hash, const char* value) {
  const CVarParameter* cvar = get_cvar(hash);
  if (cvar)
    string_cvars[cvar->array_index].current = value;
}

CVarParameter* CVarSystem::create_float_cvar(const char* name,
                                             const char* description,
                                             const float default_value,
                                             const float current_value) {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::FLOAT;
  param->array_index = (uint32_t)float_cvars.size();
  float_cvars.emplace_back(default_value, current_value, param);

  return param;
}

CVarParameter* CVarSystem::create_int_cvar(const char* name,
                                           const char* description,
                                           const int32_t default_value,
                                           const int32_t current_value) {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::INT;
  param->array_index = (uint32_t)int_cvars.size();
  int_cvars.emplace_back(default_value, current_value, param);

  return param;
}

CVarParameter* CVarSystem::create_string_cvar(const char* name,
                                              const char* description,
                                              const char* default_value,
                                              const char* current_value) {
  std::unique_lock lock(mutex_);
  CVarParameter* param = init_cvar(name, description);

  param->type = CVarType::STRING;
  param->array_index = (uint32_t)string_cvars.size();
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

AutoCVar_Float::AutoCVar_Float(const char* name,
                               const char* description,
                               const float default_value,
                               const CVarFlags flags) {
  CVarParameter* cvar = CVarSystem::get()->create_float_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}

float AutoCVar_Float::get() const { return CVarSystem::get()->float_cvars.at(index).current; }

float* AutoCVar_Float::get_ptr() const { return &CVarSystem::get()->float_cvars[index].current; }

void AutoCVar_Float::set(const float val) const { CVarSystem::get()->float_cvars.at(index).current = val; }

AutoCVar_Int::AutoCVar_Int(const char* name,
                           const char* description,
                           const int32_t default_value,
                           const CVarFlags flags) {
  CVarParameter* cvar = CVarSystem::get()->create_int_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}

int32_t AutoCVar_Int::get() const { return CVarSystem::get()->int_cvars.at(index).current; }

int32_t* AutoCVar_Int::get_ptr() const { return &CVarSystem::get()->int_cvars.at(index).current; }

void AutoCVar_Int::set(const int32_t val) const { CVarSystem::get()->int_cvars.at(index).current = val; }

void AutoCVar_Int::toggle() const {
  const bool enabled = get() != 0;

  set(enabled ? 0 : 1);
}

AutoCVar_String::AutoCVar_String(const char* name,
                                 const char* description,
                                 const char* default_value,
                                 const CVarFlags flags) {
  CVarParameter* cvar = CVarSystem::get()->create_string_cvar(name, description, default_value, default_value);
  cvar->flags = flags;
  index = cvar->array_index;
}

std::string AutoCVar_String::get() const { return CVarSystem::get()->string_cvars.at(index).current; };

void AutoCVar_String::set(std::string&& val) const { CVarSystem::get()->string_cvars.at(index).current = val; }
} // namespace ox
