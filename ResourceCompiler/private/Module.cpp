#include "ResourceCompiler.hpp"

namespace ox::rc {
auto ResourceCompiler::init(this ResourceCompiler& self) -> std::expected<void, std::string> {
  ZoneScoped;

  auto session = Session::create();
  if (!session.has_value()) {
    return std::unexpected("Failed to create the resource compiler session.");
  }

  // `Handle` is a pointer wrapper, so taking over the base subobject is the whole handoff.
  static_cast<Session&>(self) = session.value();

  return {};
}

auto ResourceCompiler::deinit(this ResourceCompiler& self) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self) {
    self.destroy();
  }

  return {};
}
} // namespace ox::rc
