#pragma once

#include <slang-com-ptr.h>

#include "ResourceCompiler.hpp"
#include "SlangVFS.hpp"

namespace ox::rc {
struct ShaderSession::Impl {
  Session rc_session = {};
  std::unique_ptr<SlangVirtualFS> virtual_fs = {};
  Slang::ComPtr<slang::ISession> slang_session = {};
  std::vector<std::string> diagnostic_messages = {};
};

} // namespace ox::rc
