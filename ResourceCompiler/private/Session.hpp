#pragma once

#include <slang-com-ptr.h>

#include "ResourceCompiler.hpp"

namespace ox::rc {
struct Session::Impl {
  Slang::ComPtr<slang::IGlobalSession> slang_global_session = {};
  std::vector<std::unique_ptr<ShaderSession::Impl>> shader_sessions = {};
};

} // namespace ox::rc
