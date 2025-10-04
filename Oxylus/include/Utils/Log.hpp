#pragma once

#include <loguru.hpp>

namespace ox {
class Log {
public:
  static void init(int argc, char** argv);
  static void shutdown();

  static void add_callback(const char* id,
                           loguru::log_handler_t callback,
                           void* user_data,
                           loguru::Verbosity verbosity,
                           loguru::close_handler_t on_close = nullptr,
                           loguru::flush_handler_t on_flush = nullptr);

  static void remove_callback(const char* id);
};
} // namespace ox

// log macros
#ifdef OX_DEBUG
  #define OX_LOG_DEBUG(...) DLOG_F(MAX, __VA_ARGS__)
#else
  #define OX_LOG_DEBUG(...)
#endif
#define OX_LOG_TRACE(...) LOG_F(MAX, __VA_ARGS__)
#define OX_LOG_INFO(...) LOG_F(INFO, __VA_ARGS__)
#define OX_LOG_WARN(...) LOG_F(WARNING, __VA_ARGS__)
#define OX_LOG_ERROR(...) LOG_F(ERROR, __VA_ARGS__)
#define OX_LOG_FATAL(...) LOG_F(FATAL, __VA_ARGS__)

#define OX_ASSERT(test, ...) CHECK_F(test, ##__VA_ARGS__)
#define OX_CHECK_NULL(test, ...) CHECK_NOTNULL_F(test, ##__VA_ARGS__)
#define OX_CHECK_EQ(a, b, ...) CHECK_EQ_F(a, b, ##__VA_ARGS__)
#define OX_CHECK_NE(a, b, ...) CHECK_NE_F(a, b, ##__VA_ARGS__)
#define OX_CHECK_LT(a, b, ...) CHECK_LT_F(a, b, ##__VA_ARGS__)
#define OX_CHECK_GT(a, b, ...) CHECK_GT_F(a, b, ##__VA_ARGS__)
#define OX_CHECK_LE(a, b, ...) CHECK_LE_F(a, b, ##__VA_ARGS__)
#define OX_CHECK_GE(a, b, ...) CHECK_GE_F(a, b, ##__VA_ARGS__)

#define OX_UNIMPLEMENTED(func, ...) LOG_F(ERROR, "Unimplemented: {} " __VA_ARGS__, #func)

#ifndef OX_DEBUG
  #define OX_DISABLE_DEBUG_BREAKS
#endif

#ifndef OX_DISABLE_DEBUG_BREAKS
  #if defined(OX_PLATFORM_WINDOWS)
    #define OX_DEBUGBREAK() __debugbreak()
  #elif defined(OX_PLATFORM_LINUX)
    #include <signal.h>
    #define OX_DEBUGBREAK() raise(SIGTRAP)
  #elif defined(OX_PLATFORM_MACOS)
    #define OX_DEBUGBREAK()
  #else
    #define OX_DEBUGBREAK()
  #endif
#else
  #define OX_DEBUGBREAK()
#endif
