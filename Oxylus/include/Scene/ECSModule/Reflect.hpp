// DLS for ECS data reflection.
//
// Paste these macros into Component header:
/*
#ifndef ECS_COMPONENT_BEGIN
#define ECS_COMPONENT_BEGIN(...)
#endif

#ifndef ECS_COMPONENT_END
#define ECS_COMPONENT_END(...)
#endif

#ifndef ECS_COMPONENT_MEMBER
#define ECS_COMPONENT_MEMBER(...)
#endif

#ifndef ECS_COMPONENT_TAG
#define ECS_COMPONENT_TAG(...)
#endif
*/

// clang-format off
#ifdef ECS_EXPORT_TYPES
#define ECS_COMPONENT_BEGIN(name, ...) struct name {
#define ECS_COMPONENT_END(...) }
#define ECS_COMPONENT_MEMBER(name, type, ...) type name __VA_OPT__(=) __VA_ARGS__;
#define ECS_COMPONENT_TAG(name, ...) struct name {}
#endif

#ifdef OX_LUA_BINDINGS
#define ECS_BIND_TYPE(state, type) auto component_type = state->new_usertype<type>(#type); \
  component_table[#type] = (u64)component.id()
#define ECS_BIND_MEMBER(name, type) component_type[#name] = &type::name
#define ECS_BIND_GET_FUNCTIONS(Component) \
    entity_type.set_function("try_get_" #Component, [](flecs::entity& e) -> const Component* { \
        return e.try_get<Component>(); \
    }); \
    entity_type.set_function("try_get_mut_" #Component, [](flecs::entity& e) -> Component* { \
        return e.try_get_mut<Component>(); \
    })
#else
#define ECS_BIND_TYPE(state, type)
#define ECS_BIND_MEMBER(name, type)
#define ECS_BIND_GET_FUNCTIONS(Component)
#endif

#ifdef ECS_REFLECT_TYPES
#define ECS_COMPONENT_BEGIN(name, ...) { \
  using _CurrentComponentT = name; \
  auto component = world.component<name>(#name); \
  ECS_BIND_TYPE(state, name); \
  ECS_BIND_GET_FUNCTIONS(name);

#define ECS_COMPONENT_END(...) }

#define ECS_COMPONENT_MEMBER(name, type, ...) \
  component.member<type, _CurrentComponentT>(#name, &_CurrentComponentT::name); \
  ECS_BIND_MEMBER(name, _CurrentComponentT);

#define ECS_COMPONENT_TAG(name, ...) \
  auto component = world.component<name>(#name); \
  ECS_BIND_TYPE(state, name);
#endif
// clang-format on
