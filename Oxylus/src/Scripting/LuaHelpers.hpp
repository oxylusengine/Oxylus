#pragma once

#include <sol/sol.hpp>

#define SET_TYPE_FIELD(var, type, field) var[#field] = &type::field
#define SET_TYPE_FUNCTION(var, type, function) var.set_function(#function, &type::function)
#define ENUM_FIELD(type, field) {#field, type::field}

#define C_NAME(c) #c

#define FIELD(type, field) #field, &type::field
#define CFIELD(type, field) .set(#field, &type::field)
#define CTOR(...) sol::constructors<sol::types<>, sol::types<__VA_ARGS__>>()
#define CTORS(...) __VA_ARGS__
