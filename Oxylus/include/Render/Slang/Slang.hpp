#pragma once
#include <vuk/vsl/Core.hpp>

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
    std::string root_directory = {};
    std::vector<std::pair<std::string, std::string>> definitions = {};
  };

  struct CompileInfo {
    std::string path = {};
    std::vector<std::string> entry_points = {};
  };

  void create_session(this Slang& self, const SessionInfo& session_info);
  void add_shader(this Slang& self, vuk::PipelineBaseCreateInfo& pipeline_ci, const CompileInfo& compile_info);

  void create_pipeline(
    this Slang& self,
    vuk::Runtime& runtime,
    const vuk::Name& name,
    const CompileInfo& compile_info,
    vuk::PersistentDescriptorSet* pds = nullptr
  );

private:
  option<SlangSession> slang_session = nullopt;
};
} // namespace ox
