#include "Core/EventSystem.hpp"

namespace ox {
EventError::EventError(Error e) : error(e) {}

auto EventError::message() -> std::string_view {
  switch (error) {
    case Error::HandlerNotFound    : return "HandlerNotFound";
    case Error::EventSystemShutdown: return "EventSystemShutdown";
    case Error::InvalidHandler     : return "InvalidHandler";
    case Error::NoHandlers         : return "NoHandlers";
  }

  OX_ASSERT(false, "Invalid EventError");
}

RegistryBase::~RegistryBase() = default;

auto EventSystem::init() -> std::expected<void, std::string> { return {}; }

auto EventSystem::deinit() -> std::expected<void, std::string> {
  shutdown();
  return {};
}

void EventSystem::shutdown() {
  ZoneScoped;
  shutdown_.store(true);

  std::unique_lock lock(registries_mutex_);
  registries_.clear();
}

bool EventSystem::is_shutdown() const { return shutdown_.load(); }
} // namespace ox
