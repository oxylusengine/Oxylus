#include <filesystem>
#include <gtest/gtest.h>

#include "Asset/AssetFile.hpp"
#include "Render/Graph/Pipeline.hpp"

namespace ox {
static auto shader_pack(std::string_view name) -> option<std::filesystem::path> {
  auto error = std::error_code{};
  for (auto directory :
       {std::filesystem::current_path(error), std::filesystem::canonical("/proc/self/exe", error).parent_path()}) {
    const auto path = directory / "Assets" / "Shaders" / name;
    if (std::filesystem::exists(path, error)) {
      return path;
    }
  }

  return nullopt;
}

// what declaring the pipeline checks, run over every pipeline the build ships
TEST(EngineShaders, EveryPipelineLayoutIsConsistentAcrossItsStages) {
  for (const auto* name : {"engine.oxpack", "editor.oxpack"}) {
    const auto path = shader_pack(name);
    ASSERT_TRUE(path.has_value()) << name << " was not built next to the test";

    const auto pack = AssetFile::unpack(*path);
    ASSERT_TRUE(pack.has_value()) << *path;
    for (const auto& entry : pack->entries) {
      const auto* pipeline = std::get_if<ShaderPipelineData>(&entry.data);
      if (!pipeline) {
        continue;
      }

      SCOPED_TRACE(pipeline->module_name);
      auto layouts = std::vector<vir::ShaderLayout>{};
      for (const auto& entry_point : pipeline->entry_points) {
        EXPECT_FALSE(entry_point.spirv.empty()) << entry_point.name;
        EXPECT_EQ(entry_point.layout.entry_point, entry_point.name);
        auto layout = entry_point.layout;
        for (auto& binding : layout.bindings) {
          EXPECT_TRUE(
            binding.set == 0 || (binding.set == 1 && (pipeline->required_features & ShaderFeatureFlag::Bindless)) ||
            (binding.set == 2 && pipeline->module_name == "resolve_shadowmaps")
          ) << "unexpected descriptor set "
            << binding.set << " at binding " << binding.binding;
          binding.access = vir::descriptor_access(binding.usage, binding.stages);
        }

        layouts.push_back(std::move(layout));
      }

      auto bindless = option<vir::BindlessDescriptorSet>{};
      if (pipeline->required_features & ShaderFeatureFlag::Bindless) {
        bindless = vir::BindlessDescriptorSet{
          .index = 1,
          .layout = reinterpret_cast<VkDescriptorSetLayout>(uintptr_t{1}),
          .set = reinterpret_cast<VkDescriptorSet>(uintptr_t{1}),
        };
      }

      EXPECT_TRUE(vir::validate_descriptor_bindings(layouts, bindless).has_value());
      const auto is_ray_tracing = std::ranges::any_of(layouts, [](const vir::ShaderLayout& layout) {
        return layout.stage == vir::ShaderStage::RayGeneration;
      });
      if (is_ray_tracing) {
        EXPECT_TRUE(vir::validate_shader_groups(layouts, vir::default_shader_groups(layouts)).has_value());
      }
    }
  }
}
} // namespace ox
