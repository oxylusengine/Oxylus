#pragma once

#include <algorithm>
#include <atomic>
#include <expected>
#include <functional>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <tracy/Tracy.hpp>
#include <typeindex>

#include "Core/Option.hpp"
#include "Utils/Log.hpp"

namespace ox {
using HandlerId = std::uint64_t;

enum class EventError { HandlerNotFound, EventSystemShutdown, InvalidHandler };

inline auto event_error_to_sv(EventError error) -> std::string_view {
  ZoneScoped;

  switch (error) {
    case EventError::HandlerNotFound    : return "HandlerNotFound";
    case EventError::EventSystemShutdown: return "EventSystemShutdown";
    case EventError::InvalidHandler     : return "InvalidHandler";
  }
}

template <typename T>
concept Event = std::is_object_v<T> && std::copyable<T>;

class RegistryBase {
public:
  virtual ~RegistryBase() = default;
};

template <Event EventType>
class HandlerRegistry : public RegistryBase {
public:
  HandlerId subscribe(std::function<void(const EventType&)> handler) {
    ZoneScoped;
    auto id = next_id_.fetch_add(1);
    auto h = std::make_shared<Handler>(std::move(handler), id);

    std::unique_lock lock(mutex_);
    handlers_.push_back(h);
    return id;
  }

  bool unsubscribe(HandlerId id) {
    ZoneScoped;
    std::shared_lock lock(mutex_);
    auto it = std::ranges::find_if(handlers_, [id](const auto& h) { return h->id == id; });

    if (it != handlers_.end()) {
      (*it)->active.store(false);
      return true;
    }
    return false;
  }

  void emit(const EventType& event) {
    ZoneScoped;
    std::shared_lock lock(mutex_);

    // Copy active handlers to avoid holding lock during callback execution
    std::vector<std::shared_ptr<Handler>> active_handlers;
    active_handlers.reserve(handlers_.size());

    std::ranges::copy_if(handlers_, std::back_inserter(active_handlers), [](const auto& h) {
      return h->active.load();
    });

    lock.unlock();

    // Execute callbacks without holding the lock
    for (const auto& handler : active_handlers) {
      if (handler->active.load()) {
        handler->callback(event);
      }
    }

    cleanup_inactive_handlers();
  }

  void clear() {
    ZoneScoped;
    std::unique_lock lock(mutex_);
    for (auto& handler : handlers_) {
      handler->active.store(false);
    }
    handlers_.clear();
  }

  std::size_t handler_count() const {
    ZoneScoped;
    std::shared_lock lock(mutex_);
    return std::ranges::count_if(handlers_, [](const auto& h) { return h->active.load(); });
  }

private:
  void cleanup_inactive_handlers() {
    ZoneScoped;
    static thread_local std::chrono::steady_clock::time_point last_cleanup{};
    auto now = std::chrono::steady_clock::now();

    // Only cleanup every 100ms to avoid excessive locking
    if (now - last_cleanup < std::chrono::milliseconds(100)) {
      return;
    }
    last_cleanup = now;

    std::unique_lock lock(mutex_);
    std::erase_if(handlers_, [](const auto& h) { return !h->active.load(); });
  }

  struct Handler {
    std::function<void(const EventType&)> callback;
    std::atomic<bool> active = {true};
    HandlerId id;

    Handler(std::function<void(const EventType&)> cb, HandlerId handler_id) : callback(std::move(cb)), id(handler_id) {}
  };

  mutable std::shared_mutex mutex_;
  std::vector<std::shared_ptr<Handler>> handlers_;
  std::atomic<HandlerId> next_id_ = {1};
};

class EventSystem {
public:
  constexpr static auto MODULE_NAME = "EventSystem";

  auto init() -> std::expected<void, std::string> { return {}; }
  auto deinit() -> std::expected<void, std::string> {
    shutdown();
    return {};
  }

  template <Event EventType>
  std::expected<HandlerId, EventError> subscribe(std::function<void(const EventType&)> handler) {
    ZoneScoped;
    if (shutdown_.load()) {
      return std::unexpected(EventError::EventSystemShutdown);
    }

    if (!handler) {
      return std::unexpected(EventError::InvalidHandler);
    }

    auto* registry = get_registry<EventType>();
    return registry->subscribe(std::move(handler));
  }

  template <Event EventType, typename Callable>
    requires std::invocable<Callable, const EventType&>
  std::expected<HandlerId, EventError> subscribe(Callable&& callable) {
    return subscribe<EventType>(std::function<void(const EventType&)>(std::forward<Callable>(callable)));
  }

  template <Event EventType>
  std::expected<void, EventError> unsubscribe(HandlerId id) {
    ZoneScoped;
    if (shutdown_.load()) {
      return std::unexpected(EventError::EventSystemShutdown);
    }

    std::shared_lock lock(registries_mutex_);
    auto type_idx = std::type_index(typeid(EventType));

    if (auto it = registries_.find(type_idx); it != registries_.end()) {
      auto* registry = static_cast<HandlerRegistry<EventType>*>(it->second.get());
      if (registry->unsubscribe(id)) {
        return {};
      }
    }

    return std::unexpected(EventError::HandlerNotFound);
  }

  template <Event EventType>
  void emit(const EventType& event) {
    ZoneScoped;
    if (shutdown_.load()) {
      return;
    }

    auto* registry = get_registry<EventType>();
    registry->emit(event);
  }

  template <Event EventType>
  void emit(EventType&& event) {
    emit<EventType>(static_cast<const EventType&>(event));
  }

  template <Event EventType>
  std::size_t handler_count() const {
    ZoneScoped;
    std::shared_lock lock(registries_mutex_);
    auto type_idx = std::type_index(typeid(EventType));

    if (auto it = registries_.find(type_idx); it != registries_.end()) {
      auto* registry = static_cast<HandlerRegistry<EventType>*>(it->second.get());
      return registry->handler_count();
    }
    return 0;
  }

  void shutdown() {
    ZoneScoped;
    shutdown_.store(true);

    std::unique_lock lock(registries_mutex_);
    registries_.clear();
  }

  bool is_shutdown() const { return shutdown_.load(); }

private:
  mutable std::shared_mutex registries_mutex_;
  std::unordered_map<std::type_index, std::unique_ptr<RegistryBase>> registries_;
  std::atomic<bool> shutdown_ = {false};

  template <Event EventType>
  HandlerRegistry<EventType>* get_registry() {
    ZoneScoped;
    std::shared_lock lock(registries_mutex_);
    auto type_idx = std::type_index(typeid(EventType));

    if (auto it = registries_.find(type_idx); it != registries_.end()) {
      return static_cast<HandlerRegistry<EventType>*>(it->second.get());
    }

    lock.unlock();
    std::unique_lock write_lock(registries_mutex_);

    if (auto it = registries_.find(type_idx); it != registries_.end()) {
      return static_cast<HandlerRegistry<EventType>*>(it->second.get());
    }

    auto registry = std::make_unique<HandlerRegistry<EventType>>();
    auto* ptr = registry.get();

    registries_[type_idx] = std::move(registry);

    return ptr;
  }
};

template <Event EventType>
class ScopedSubscription {
public:
  ScopedSubscription(EventSystem* sys, HandlerId id) : system_(sys), handler_id_(id), active_(true) {}

  ~ScopedSubscription() { unsubscribe(); }

  ScopedSubscription(const ScopedSubscription&) = delete;
  ScopedSubscription& operator=(const ScopedSubscription&) = delete;

  ScopedSubscription(ScopedSubscription&& other) noexcept
      : system_(other.system_),
        handler_id_(other.handler_id_),
        active_(other.active_) {
    other.active_ = false;
  }

  ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
    if (this != &other) {
      unsubscribe();
      system_ = other.system_;
      handler_id_ = other.handler_id_;
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }

  void unsubscribe() {
    ZoneScoped;
    if (active_ && system_) {
      auto unsubscribe_result = system_->unsubscribe<EventType>(handler_id_);
      if (!unsubscribe_result) {
        OX_LOG_ERROR("{}", event_error_to_sv(unsubscribe_result.error()));
      }
      active_ = false;
    }
  }

  bool is_active() const { return active_; }
  HandlerId id() const { return handler_id_; }

private:
  EventSystem* system_;
  HandlerId handler_id_;
  bool active_;
};

template <Event EventType, typename Callable>
auto make_scoped_subscription(EventSystem& system, Callable&& handler) -> option<ScopedSubscription<EventType>> {
  auto result = system.subscribe<EventType>(std::forward<Callable>(handler));
  if (result) {
    return ScopedSubscription<EventType>(&system, *result);
  }
  return nullopt;
}
} // namespace ox
