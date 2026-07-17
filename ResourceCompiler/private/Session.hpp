#pragma once

#include <shared_mutex>
#include <slang-com-ptr.h>
#include <string>

#include "ResourceCompiler.hpp"

namespace ox {
template <>
struct Handle<rc::Session>::Impl {
  u16 version = 1;
  std::shared_mutex messages_mutex = {};
  std::vector<std::string> errors = {};
  std::vector<std::string> messages = {};

  std::shared_mutex packers_mutex = {};
  std::vector<std::unique_ptr<rc::Packer::Impl>> packers = {};

  std::shared_mutex session_mutex = {};
  Slang::ComPtr<slang::IGlobalSession> slang_global_session = {};
};
} // namespace ox
