#include "ResourceCompiler.hpp"
#include "Session.hpp"

namespace ox::rc {
auto ResourceCompiler::init(this ResourceCompiler& self) -> std::expected<void, std::string> {
  auto session = Session::create();
  // fuck, it is what it is
  std::memcpy(&self, &session, sizeof(Session));

  if (!self->init_internal()) {
    return std::unexpected("Failed to initialize Resource Compiler");
  }

  return {};
}

auto ResourceCompiler::deinit(this ResourceCompiler& self) -> std::expected<void, std::string> {
  self.destroy();

  return {};
}
} // namespace ox::rc
