#include "Scripting/LuaFlecsBindings.hpp"

#include <flecs.h>
#include <flecs/addons/meta.h>
#include <sol/state.hpp>

#include "Core/Types.hpp"
#include "Memory/Stack.hpp"
#include "Scene/EntitySerializer.hpp"
#include "Scene/Scene.hpp"

struct ecs_world_t {};

namespace ox {
struct LuaEntitySerializer : IEntitySerializer {
  sol::state* state;
  sol::table& table;
  bool is_mutable;
  memory::ScopedStack stack;

  LuaEntitySerializer(flecs::world& world_, sol::state* state_, sol::table& table_, bool is_mutable_)
      : IEntitySerializer(world_),
        state(state_),
        table(table_),
        is_mutable(is_mutable_) {}

  auto on_primitive(std::string_view name, Primitive primitive) -> void override {
    ZoneScoped;

    std::visit(
      ox::match{
        [](const auto&) {},
        [&](bool* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, bool new_value) {
              *v = new_value;
            });
          }
        },
        [&](c8* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, c8 new_value) {
              *v = new_value;
            });
          }
        },
        [&](i8* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, i8 new_value) {
              *v = new_value;
            });
          }
        },
        [&](u8* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, u8 new_value) {
              *v = new_value;
            });
          }
        },
        [&](i16* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, i16 new_value) {
              *v = new_value;
            });
          }
        },
        [&](u16* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, u16 new_value) {
              *v = new_value;
            });
          }
        },
        [&](i32* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, i32 new_value) {
              *v = new_value;
            });
          }
        },
        [&](u32* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, u32 new_value) {
              *v = new_value;
            });
          }
        },
        [&](i64* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, i64 new_value) {
              *v = new_value;
            });
          }
        },
        [&](u64* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, u64 new_value) {
              *v = new_value;
            });
          }
        },
        [&](f32* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, f32 new_value) {
              *v = new_value;
            });
          }
        },
        [&](f64* v) {
          table[name] = *v;
          if (is_mutable) {
            table.set_function(stack.format_char("set_{}", name), [v](const sol::table&, f64 new_value) {
              *v = new_value;
            });
          }
        },
      },
      primitive
    );
  }

  auto on_string(std::string_view name, const c8** str) -> void override {
    ZoneScoped;

    if (str && *str) {
      table[name] = *str;
      if (is_mutable) {
        table.set_function(stack.format_char("set_{}", name), [str](const sol::table&, const std::string& new_value) {
          *const_cast<const c8**>(str) = new_value.c_str();
        });
      }
    }
  }

  auto on_entity(std::string_view name, flecs::entity* entity) -> void override {
    ZoneScoped;

    if (entity && entity->is_valid()) {
      table[name] = *entity;
      if (is_mutable) {
        table.set_function(stack.format_char("set_{}", name), [entity](const sol::table&, flecs::entity new_value) {
          *entity = new_value;
        });
      }
    }
  }

  auto on_component(std::string_view name, flecs::id_t* component) -> void override {
    ZoneScoped;

    if (component && *component) {
      flecs::entity comp_entity(world, *component);
      if (comp_entity.is_valid()) {
        table[name] = comp_entity;
        if (is_mutable) {
          table.set_function(
            stack.format_char("set_{}", name),
            [component](const sol::table&, flecs::entity new_value) { *component = new_value.id(); }
          );
        }
      }
    }
  }

  auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override {
    ZoneScoped;

    if (!name.empty()) {
      sol::table nested_table = state->create_table();
      auto nested_serializer = LuaEntitySerializer(world, state, nested_table, is_mutable);
      nested_serializer.serialize_ops(ops + 1, op_count - 1, base);
      table[name] = nested_table;
    } else {
      serialize_ops(ops + 1, op_count - 1, base);
    }
  }

  auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void override {
    ZoneScoped;

    auto* opaque_info = ecs_get(world, field_type, EcsOpaque);

    if (opaque_type == flecs::Bool) {
      table[name] = *static_cast<const bool*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_bool) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, bool new_value) {
            opaque_info->assign_bool(field_ptr, new_value);
          }
        );
      }
    } else if (opaque_type == flecs::Char) {
      table[name] = *static_cast<const c8*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_char) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, c8 new_value) { opaque_info->assign_char(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::Byte || opaque_type == flecs::U8) {
      table[name] = *static_cast<const u8*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_uint) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, u64 new_value) { opaque_info->assign_uint(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::U16) {
      table[name] = *static_cast<const u16*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_uint) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, u64 new_value) { opaque_info->assign_uint(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::U32) {
      table[name] = *static_cast<const u32*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_uint) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, u64 new_value) { opaque_info->assign_uint(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::U64 || opaque_type == flecs::Uptr) {
      table[name] = *static_cast<const u64*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_uint) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, u64 new_value) { opaque_info->assign_uint(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::I8) {
      table[name] = *static_cast<const i8*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_int) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, i64 new_value) { opaque_info->assign_int(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::I16) {
      table[name] = *static_cast<const i16*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_int) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, i64 new_value) { opaque_info->assign_int(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::I32) {
      table[name] = *static_cast<const i32*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_int) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, i64 new_value) { opaque_info->assign_int(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::I64 || opaque_type == flecs::Iptr) {
      table[name] = *static_cast<const i64*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_int) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, i64 new_value) { opaque_info->assign_int(field_ptr, new_value); }
        );
      }
    } else if (opaque_type == flecs::F32) {
      table[name] = *static_cast<const f32*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_float) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, f64 new_value) {
            opaque_info->assign_float(field_ptr, new_value);
          }
        );
      }
    } else if (opaque_type == flecs::F64) {
      table[name] = *static_cast<const f64*>(value);
      if (is_mutable && opaque_info && opaque_info->assign_float) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, f64 new_value) {
            opaque_info->assign_float(field_ptr, new_value);
          }
        );
      }
    } else if (opaque_type == flecs::String) {
      const auto* str = *static_cast<const c8* const*>(value);
      table[name] = str ? str : "";
      if (is_mutable && opaque_info && opaque_info->assign_string) {
        table.set_function(
          stack.format_char("set_{}", name),
          [field_ptr, opaque_info](const sol::table&, const std::string& new_value) {
            opaque_info->assign_string(field_ptr, new_value.c_str());
          }
        );
      }
    }
  }
};

static auto get_component_table(
  sol::state* state, flecs::world& world, ecs_entity_t component_ty, void* component_ptr, bool is_mutable
) -> sol::table {
  ZoneScoped;

  sol::table result = state->create_table();
  result["component_id"] = component_ty;

  if (component_ptr) {
    auto serializer = LuaEntitySerializer(world, state, result, is_mutable);
    serializer.serialize(flecs::entity(world, component_ty), component_ptr);
  }

  return result;
}

auto FlecsBinding::bind(sol::state* state) -> void {
  ZoneScoped;

  auto flecs_table = state->create_named_table("flecs");

  // Phases
  flecs_table.set("OnStart", EcsOnStart);
  flecs_table.set("PreFrame", EcsPreFrame);
  flecs_table.set("OnLoad", EcsOnLoad);
  flecs_table.set("PostLoad", EcsPostLoad);
  flecs_table.set("PreUpdate", EcsPreUpdate);
  flecs_table.set("OnUpdate", EcsOnUpdate);
  flecs_table.set("OnValidate", EcsOnValidate);
  flecs_table.set("PostUpdate", EcsPostUpdate);
  flecs_table.set("PreStore", EcsPreStore);
  flecs_table.set("OnStore", EcsOnStore);
  flecs_table.set("PostFrame", EcsPostFrame);

  flecs_table.set("Exclusive", EcsExclusive);
  flecs_table.set("IsA", EcsIsA);

  // --- entity_t ---
  auto id_type = flecs_table.new_usertype<flecs::entity_t>("entity_t");

  // ecs_iter_t
  auto iter_type = flecs_table.new_usertype<ecs_iter_t>(
    "iter",

    sol::call_constructor,
    [](ecs_query_t* q) { return ecs_query_iter(q->world, q); },

    "count",
    [](ecs_iter_t* it) -> int32_t { return it->count; },

    "field",
    [state](ecs_iter_t* it, i32 index, sol::table component_table) {
      auto component = component_table.get<ecs_entity_t>("component_id");
      sol::table result = state->create_table();
      result["component_id"] = component;

      result.set_function(
        "at",
        [it, state](const sol::table& self, int i) -> sol::table {
          ecs_entity_t c = self["component_id"];

          OX_CHECK_LT(i, it->count);
          auto entity = it->entities[i];

          auto e = flecs::entity{it->real_world, entity};
          flecs::world world = e.world();

          return get_component_table(state, world, c, e.get_mut(c), true);
        }

      );

      return result;
    },

    "entity",
    [](ecs_iter_t* it, i32 i) -> flecs::entity {
      OX_CHECK_LT(i, it->count);
      auto entity = it->entities[i];

      auto e = flecs::entity{it->real_world, entity};
      return e;
    },

    "query_next",
    [](ecs_iter_t* it) -> bool { return ecs_query_next(it); }
  );

  // --- world ---
  auto world_type = flecs_table.new_usertype<ecs_world_t>(
    "world",

    "component",
    [](ecs_world_t* w, sol::table component_table) -> flecs::entity {
      auto component = component_table.get<ecs_entity_t>("component_id");

      return flecs::entity{w, component};
    },

    "entity",
    [](ecs_world_t* w, const sol::optional<std::string> name) -> flecs::entity {
      flecs::entity e = {};
      flecs::world cpp_world(w);
      if (name.has_value())
        e = cpp_world.entity(name->c_str());
      else
        e = cpp_world.entity();
      return e;
    },

    "system",
    [state](
      ecs_world_t* world,
      const std::string& name,
      sol::table components,
      sol::table dependencies,
      sol::function callback
    ) -> sol::table {
      std::vector<ecs_entity_t> component_ids = {};
      component_ids.reserve(components.size());
      components.for_each([&](sol::object key, sol::object value) {
        sol::table component_table = value.as<sol::table>();
        component_ids.emplace_back(component_table["component_id"].get<ecs_entity_t>());
      });

      std::vector<ecs_id_t> dependency_ids = {};
      dependency_ids.reserve(dependencies.size());
      dependencies.for_each([&](sol::object key, sol::object value) {
        ecs_entity_t dep = value.as<ecs_entity_t>();
        dependency_ids.emplace_back(ecs_dependson(dep));
      });

      ecs_system_desc_t system_desc = {};

      ecs_entity_desc_t entity_desc = {};
      entity_desc.name = name.c_str();
      entity_desc.add = dependency_ids.data();

      system_desc.entity = ecs_entity_init(world, &entity_desc);

      system_desc.callback_ctx = new std::shared_ptr<sol::function>(new sol::function(callback));
      system_desc.callback_ctx_free = [](void* ctx) {
        delete reinterpret_cast<std::shared_ptr<sol::function>*>(ctx);
      };
      system_desc.callback = [](ecs_iter_t* it) {
        auto lua_callback = reinterpret_cast<std::shared_ptr<sol::function>*>(it->callback_ctx);

        OX_CHECK_NULL(lua_callback);
        OX_CHECK_EQ((*lua_callback)->valid(), true);

        auto result = (**lua_callback)(it);
        if (!result.valid()) {
          sol::error err = result;
          OX_LOG_ERROR("Lua lambda function error: {}", err.what());
        }
      };

      for (usize i = 0; i < component_ids.size(); i++) {
        system_desc.query.terms[i].id = component_ids[i];
      }

      auto system_table = state->create_table();
      system_table["system"] = ecs_system_init(world, &system_desc);

      return system_table;
    },

    "query",
    [](ecs_world_t* world, sol::table components) {
      std::vector<ecs_entity_t> component_ids = {};
      std::vector<std::pair<ecs_entity_t, ecs_entity_t>> pair_component_ids = {};
      component_ids.reserve(components.size());

      components.for_each([&](sol::object key, sol::object value) {
        sol::table component_table = value.as<sol::table>();
        if (auto c_id = component_table["component_id"].get<sol::optional<ecs_entity_t>>()) {
          component_ids.emplace_back(*c_id);
        } else {
          std::pair<ecs_entity_t, ecs_entity_t> pair = {};
          u32 index = 0;
          component_table.for_each([&](sol::object k, sol::object v) {
            ecs_entity_t c = {};
            if (auto component_id = v.as<sol::table>().get<sol::optional<ecs_entity_t>>("component_id")) {
              c = *component_id;
            } else {
              c = v.as<flecs::entity>();
            }
            index == 0 ? pair.first = c : pair.second = c;
            index += 1;
          });
          pair_component_ids.emplace_back(pair);
        }
      });

      ecs_query_desc_t desc = {};
      for (usize i = 0; i < component_ids.size(); i++) {
        desc.terms[i].id = component_ids[i];
      }

      for (usize i = component_ids.size(); i < pair_component_ids.size(); i++) {
        desc.terms[i].first.id = pair_component_ids[i].first;
        desc.terms[i].second.id = pair_component_ids[i].second;
      }

      ecs_query_t* q = ecs_query_init(world, &desc);
      return q;
    },

    "defer",
    [](ecs_world_t* w, sol::function func) {
      flecs::world cpp_world(w);
      cpp_world.defer([func]() { func(); });
    },

    "defer_resume",
    [](ecs_world_t* w) {
      flecs::world cpp_world(w);
      cpp_world.defer_resume();
    },

    "defer_suspend",
    [](ecs_world_t* w) {
      flecs::world cpp_world(w);
      cpp_world.defer_suspend();
    },

    "defer_begin",
    [](ecs_world_t* w) {
      flecs::world cpp_world(w);
      cpp_world.defer_begin();
    },

    "defer_end",
    [](ecs_world_t* w) {
      flecs::world cpp_world(w);
      cpp_world.defer_end();
    }
  );

  // --- entity ---
  auto entity_type = state->new_usertype<flecs::entity>(
    "entity",

    "id",
    [](flecs::entity* e) -> flecs::entity_t { return e->id(); },

    "name",
    [](flecs::entity* e) -> std::string { return e->name().c_str(); },

    "path",
    [](flecs::entity* e) -> std::string { return e->path().c_str(); },

    "add",
    sol::overload(
      [](flecs::entity* e, sol::table component_table, sol::optional<sol::table> values = {}) -> flecs::entity* {
        auto component = component_table.get<ecs_entity_t>("component_id");
        e->add(component);

        auto* ptr = e->try_get_mut(component);
        if (!ptr)
          return e;

        const auto set_member_value = [](flecs::cursor& cur, sol::object& value) {
          if (value.is<f64>())
            cur.set_float(value.as<f64>());
          else if (value.is<bool>())
            cur.set_float(value.as<bool>());
          else if (value.is<std::string>())
            cur.set_string(value.as<std::string>().c_str());

          else if (value.is<glm::vec2>()) {
            void* member_ptr = cur.get_ptr();
            auto member_value = value.as<glm::vec2>();
            std::memcpy(member_ptr, &member_value, sizeof(glm::vec2));
          } else if (value.is<glm::ivec2>()) {
            void* member_ptr = cur.get_ptr();
            auto member_value = value.as<glm::ivec2>();
            std::memcpy(member_ptr, &member_value, sizeof(glm::ivec2));
          } else if (value.is<glm::vec3>()) {
            void* member_ptr = cur.get_ptr();
            auto member_value = value.as<glm::vec3>();
            std::memcpy(member_ptr, &member_value, sizeof(glm::vec3));
          } else if (value.is<glm::vec4>()) {
            void* member_ptr = cur.get_ptr();
            auto member_value = value.as<glm::vec4>();
            std::memcpy(member_ptr, &member_value, sizeof(glm::vec4));
          }
        };

        if (values) {
          values->for_each([&](sol::object key, sol::object value) {
            std::string field_name = key.as<std::string>();

            flecs::cursor cur = e->world().cursor(component, ptr);
            cur.push();
            cur.member(field_name.c_str());

            set_member_value(cur, value);

            cur.pop();
          });
        } else {
          if (auto defaults = component_table.get<sol::optional<sol::table>>("defaults")) {
            defaults->for_each([&](sol::object key, sol::object value) {
              std::string field_name = key.as<std::string>();
              flecs::cursor cur = e->world().cursor(component, ptr);
              cur.push();
              cur.member(field_name.c_str());

              set_member_value(cur, value);

              cur.pop();
            });
          }
        }

        return e;
      },
      [](flecs::entity* e, flecs::entity other) -> flecs::entity* {
        e->add(other);
        return e;
      },
      [](flecs::entity* e, ecs_entity_t other) -> flecs::entity* {
        e->add(other);
        return e;
      }
    ),

    "add_pair",
    [](flecs::entity* e, sol::object first, sol::object second) -> flecs::entity* {
      ecs_entity_t first_component = {};
      ecs_entity_t second_component = {};

      if (auto component_id = first.as<sol::table>().get<sol::optional<ecs_entity_t>>("component_id")) {
        first_component = flecs::entity{e->world().world_, *component_id};
      } else {
        first_component = first.as<flecs::entity>();
      }

      if (auto component_id = second.as<sol::table>().get<sol::optional<ecs_entity_t>>("component_id")) {
        second_component = flecs::entity{e->world().world_, *component_id};
      } else {
        second_component = second.as<flecs::entity>();
      }

      ecs_add_pair(e->world().world_, *e, first_component, second_component);
      return e;
    },

    "remove",
    [](flecs::entity* e, sol::table component_table) {
      auto component = component_table.get<ecs_entity_t>("component_id");
      e->remove(component);
      return e;
    },

    "has",
    [](flecs::entity* e, sol::table component_table) -> bool {
      auto component = component_table.get<ecs_entity_t>("component_id");
      return e->has(component);
    },

    "get",
    [state](flecs::entity* e, sol::table component_table) -> sol::optional<sol::table> {
      auto component = component_table.get<ecs_entity_t>("component_id");
      if (!e->has(component))
        return sol::nullopt;

      flecs::world world = e->world();
      return get_component_table(state, world, component, const_cast<void*>(e->get(component)), false);
    },

    "get_mut",
    [state](flecs::entity* e, sol::table component_table) -> sol::optional<sol::table> {
      auto component = component_table.get<ecs_entity_t>("component_id");
      if (!e->has(component))
        return sol::nullopt;

      flecs::world world = e->world();
      return get_component_table(state, world, component, e->get_mut(component), true);
    },

    "ensure",
    [state](flecs::entity* e, sol::table component_table) -> sol::optional<sol::table> {
      auto component = component_table.get<ecs_entity_t>("component_id");
      e->ensure(component);

      flecs::world world = e->world();
      return get_component_table(state, world, component, e->get_mut(component), true);
    },

    // only available with default values
    "set",
    [](flecs::entity* e, sol::table component_table) {
      auto component = component_table.get<ecs_entity_t>("component_id");
      e->set(component);
      // ecs_set_id(e->world().world_, (ecs_entity_t)e->id(), component, 0, nullptr);
      return e;
    },

    "modified",
    [](flecs::entity* e, sol::table component_table) -> void {
      auto comp = component_table.get<ecs_entity_t>("component_id");

      e->modified(comp);
    },

    "child_of",
    [](flecs::entity* e, flecs::entity e2) { e->child_of(e2); },

    "set_name",
    [](flecs::entity* e, const std::string& name) { e->set_name(name.c_str()); },

    "destruct",
    [](flecs::entity* e) -> void { e->destruct(); },

    "parent",
    [](flecs::entity* e) -> flecs::entity { return e->parent(); },

    "is_a",
    sol::overload(
      [](flecs::entity* e, flecs::entity other) -> flecs::entity* {
        e->add(flecs::IsA, other);
        return e;
      },
      [](flecs::entity* e, sol::table component_table) -> flecs::entity* {
        auto comp = component_table.get<ecs_entity_t>("component_id");
        e->add(flecs::IsA, comp);
        return e;
      }
    ),

    "target",
    [](flecs::entity* e, sol::object component_obj) -> flecs::entity {
      ecs_entity_t component = {};
      if (auto component_id = component_obj.as<sol::table>().get<sol::optional<ecs_entity_t>>("component_id"))
        component = flecs::entity{e->world().world_, *component_id};
      else
        component = component_obj.as<flecs::entity>();

      return e->target(component);
    }
  );

  // --- Components ---
  auto components_table = state->create_named_table("Component");
  components_table["lookup"] = [state](Scene* scene, const std::string& name) -> sol::table {
    auto component = scene->world.component(name.c_str());
    sol::table component_table = state->create_table();
    component_table["component_id"] = ecs_entity_t(component);

    (*state)[name] = component_table;
    return component_table;
  };
  components_table["define"] =
    [state](Scene* scene, const std::string& name, sol::optional<sol::table> properties = {}) -> sol::table {
    auto component = scene->world.component(name.c_str());

    sol::table defaults = state->create_table();

    if (properties) {
      properties->for_each([&](sol::object key, sol::object value) {
        std::string field_name = key.as<std::string>();

        // explicit types
        if (value.is<sol::table>()) {
          sol::table field_def = value.as<sol::table>();
          std::string type = field_def["type"];
          sol::object default_val = field_def["default"];

          if (type == "f32") {
            component.member<f32>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "f64") {
            component.member<f64>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "i8") {
            component.member<i8>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "i16") {
            component.member<i16>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "i32") {
            component.member<i32>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "i64") {
            component.member<i64>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "u8") {
            component.member<u8>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "u16") {
            component.member<u16>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "u32") {
            component.member<u32>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "u64") {
            component.member<u64>(field_name.c_str());
            defaults[field_name] = default_val.as<f64>();
          } else if (type == "vec2") {
            component.member<glm::vec2>(field_name.c_str());
            defaults[field_name] = default_val.as<glm::vec3>();
          } else if (type == "vec3") {
            component.member<glm::vec3>(field_name.c_str());
            defaults[field_name] = default_val.as<glm::vec3>();
          } else if (type == "vec4") {
            component.member<glm::vec4>(field_name.c_str());
            defaults[field_name] = default_val.as<glm::vec3>();
          }
        }

        // default types
        if (value.is<f64>()) {
          component.member<f64>(field_name.c_str());
          defaults[field_name] = value.as<f64>();
        } else if (value.is<bool>()) {
          component.member<bool>(field_name.c_str());
          defaults[field_name] = value.as<bool>();
        } else if (value.is<std::string>()) {
          component.member<std::string>(field_name.c_str());
          defaults[field_name] = value.as<std::string>();
        }
      });
    }

    if (!scene->component_db.is_component_known(component))
      scene->component_db.components.emplace_back(component);

    sol::table component_table = state->create_table();
    component_table["component_id"] = ecs_entity_t(component);
    component_table["defaults"] = defaults;

    (*state)[name] = component_table;
    return component_table;
  };

  components_table["undefine"] = [](Scene* scene, sol::table component_table) -> void {
    auto component = component_table.get<ecs_entity_t>("component_id");
    ecs_delete(scene->world.world_, component);
  };
}
} // namespace ox
