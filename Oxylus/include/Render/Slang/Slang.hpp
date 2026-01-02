#pragma once

#include "Compiler.hpp"
#include "Core/Option.hpp"

namespace ox {
class Slang {
public:
  enum OptimizationLevel : i32 {
    None = 0,
    Default,
    High,
    Maximal,
  };

  struct SessionInfo {
    OptimizationLevel optimization_level = OptimizationLevel::Maximal;
    std::filesystem::path root_directory = {};
    std::vector<std::pair<std::string, std::string>> definitions = {};
    bool debug_symbols = false;
  };

  struct CompileInfo {
    std::string module_name = {};
    std::filesystem::path path = {};
    std::vector<std::string> entry_points = {};
  };

  void create_session(this Slang& self, const SessionInfo& session_info);

private:
  option<SlangSession> slang_session = nullopt;
};
} // namespace ox
