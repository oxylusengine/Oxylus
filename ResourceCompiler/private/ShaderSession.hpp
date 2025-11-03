#pragma once

#include <slang-com-ptr.h>
#include <slang.h>

#include "ResourceCompiler.hpp"
#include "SlangVFS.hpp"

namespace ox::rc {
struct ShaderSession::Impl {
  Session rc_session = {};
  std::string name = {};
  std::unique_ptr<SlangVirtualFS> virtual_fs = {};
  ankerl::unordered_dense::map<std::filesystem::path, slang::IModule*> cached_modules = {};
  Slang::ComPtr<slang::ISession> slang_session = {};
};

} // namespace ox::rc
