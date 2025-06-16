#pragma once
#include <vuk/vsl/Core.hpp>

#include "Compiler.hpp"
#include "Core/Option.hpp"

namespace ox {
class Slang {
public:
  struct SessionInfo {
    std::string root_directory = {};
    std::vector<std::pair<std::string, std::string>> definitions = {};
  };

  struct CompileInfo {
    std::string path = {};
    std::vector<std::string> entry_points = {};
  };

  void create_session(this Slang& self,
                      const SessionInfo& session_info);
  void add_shader(this Slang& self,
                  vuk::PipelineBaseCreateInfo& pipeline_ci,
                  const CompileInfo& compile_info);

  void create_pipeline(this Slang& self,
                       vuk::Runtime& runtime,
                       const vuk::Name& name,
                       const option<vuk::DescriptorSetLayoutCreateInfo>& dci,
                       const CompileInfo& compile_info);

private:
  option<SlangSession> slang_session = nullopt;
};
} // namespace ox
