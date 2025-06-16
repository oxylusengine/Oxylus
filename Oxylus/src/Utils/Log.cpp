#include "Log.hpp"

namespace ox {
void Log::init(int argc, char** argv) {
  ZoneScoped;
  if (!std::filesystem::exists("logs"))
    std::filesystem::create_directory("logs");

#ifdef OX_RELEASE
  loguru::g_stderr_verbosity = loguru::Verbosity_INFO;
#else
  loguru::g_stderr_verbosity = loguru::Verbosity_MAX;
#endif

  loguru::g_preamble_date = false;

  loguru::init(argc, argv, {});

  // Put every log message in "everything.log":
  loguru::add_file("logs/everything.log", loguru::Append, loguru::Verbosity_MAX);

  // Only log INFO, WARNING, ERROR and FATAL to "latest_readable.log":
  loguru::add_file("logs/latest.log", loguru::Truncate, loguru::Verbosity_INFO);
}
void Log::shutdown() { loguru::shutdown(); }

void Log::add_callback(const char* id,
                       loguru::log_handler_t callback,
                       void* user_data,
                       loguru::Verbosity verbosity,
                       loguru::close_handler_t on_close,
                       loguru::flush_handler_t on_flush) {
  loguru::add_callback(id, callback, user_data, verbosity, on_close, on_flush);
}

void Log::remove_callback(const char* id) { loguru::remove_callback(id); }
} // namespace ox
