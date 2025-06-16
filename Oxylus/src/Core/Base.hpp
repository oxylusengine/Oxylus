#pragma once
#define BIT(x) (1 << x)

#define DELETE_DEFAULT_CONSTRUCTORS(struct)        \
  struct(const struct& other) = delete;            \
  struct(struct && other) = delete;                \
  struct& operator=(const struct& other) = delete; \
  struct& operator=(struct&& other) = delete;

#define OX_CRASH() *(volatile int*)(nullptr) = 0;

#define OX_EXPAND_IMPL(x) x
#define OX_STRINGIFY_IMPL(x) #x
#define OX_EXPAND_STRINGIFY(x) OX_STRINGIFY_IMPL(x)
#define OX_CONCAT_IMPL(a, b) a##b
#define OX_CONCAT(x, y) OX_CONCAT_IMPL(x, y)
#define OX_UNIQUE_VAR() OX_CONCAT(_ls_v_, __COUNTER__)

#define TRY(...)                    \
  try {                             \
    __VA_ARGS__;                    \
  } catch (std::exception & exc) {  \
    OX_LOG_ERROR("{}", exc.what()); \
  }

namespace ox {
template <typename Fn>
struct defer {
  Fn func;

  defer(Fn func_) : func(std::move(func_)) {}

  ~defer() { func(); }
};

#define OX_DEFER(...) ::ox::defer OX_UNIQUE_VAR() = [__VA_ARGS__]

} // namespace ox
