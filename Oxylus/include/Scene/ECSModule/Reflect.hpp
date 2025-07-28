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
#define ECS_BIND_TYPE(state, type) auto component_table = state->create_named_table(#type); \
  component_table["component_id"] = (u64)component.id(); \
  core_table[#type] = component_table
#define ECS_BIND_MEMBER(name, type) component_table[#name] = &type::name
#else
#define ECS_BIND_TYPE(state, type)
#define ECS_BIND_MEMBER(name, type)
#endif

#ifdef ECS_REFLECT_TYPES
#define ECS_COMPONENT_BEGIN(name, ...) { \
  using _CurrentComponentT = name; \
  auto component = world.component<name>(#name); \
  ECS_BIND_TYPE(state, name); \

#define ECS_COMPONENT_END(...) }

#define ECS_COMPONENT_MEMBER(name, type, ...) \
  component.member<type, _CurrentComponentT>(#name, &_CurrentComponentT::name); \
  ECS_BIND_MEMBER(name, _CurrentComponentT);

#define ECS_COMPONENT_TAG(name, ...) \
  auto component = world.component<name>(#name); \
  ECS_BIND_TYPE(state, name);
#endif
// clang-format on
