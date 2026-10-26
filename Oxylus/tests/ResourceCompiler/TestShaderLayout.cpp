#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string_view>
#include <vir/Types.hpp>

#include "Asset/AssetFile.hpp"
#include "ResourceCompiler.hpp"

namespace ox {
constexpr auto LAYOUT_SOURCE = std::string_view{R"(
struct Push {
  float4 color;
  uint index;
};

struct Params {
  RWStructuredBuffer<uint> values;
};

struct VertexInput {
  float3 position;
  float2 uv;
  uint color;
};

struct VertexOutput {
  float4 position : SV_Position;
  float2 uv;
};

[[vk::push_constant]] Push C;
[[vk::binding(0, 0)]] StructuredBuffer<uint> input;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> output;
[[vk::binding(2, 0)]] Texture2D<float4> sampled;
[[vk::binding(3, 0)]] RWTexture2D<float4> storage;
[[vk::binding(4, 0)]] SamplerState linear_sampler;
[[vk::binding(5, 0)]] ConstantBuffer<Push> uniforms;
[[vk::binding(6, 0)]] StructuredBuffer<uint> never_read;
[[vk::binding(0, 1)]] Texture2D<float4> bindless[];
[[vk::constant_id(7)]] const uint SPEC = 1;

[shader("compute")]
[numthreads(8, 4, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
  output[id.x] = input[id.x] + C.index + SPEC + uniforms.index;
  storage[id.xy] = sampled.SampleLevel(linear_sampler, float2(0.0, 0.0), 0.0) + bindless[C.index].Load(int3(0, 0, 0)) + C.color;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void pb_main(uniform ParameterBlock<Params> params, uniform uint offset) {
  params.values[0] = offset;
}

[shader("vertex")]
VertexOutput vs_main(VertexInput vertex, uint vertex_id : SV_VertexID) {
  var result = VertexOutput();
  result.position = float4(vertex.position, float(vertex.color + vertex_id));
  result.uv = vertex.uv;
  return result;
}
)"};

static auto compile_layouts() -> std::vector<ShaderEntryPointData> {
  const auto directory = std::filesystem::temp_directory_path() / "ox_test_shader_layout";
  std::filesystem::create_directories(directory);
  std::ofstream(directory / "layout.slang") << LAYOUT_SOURCE;

  auto session = rc::Session::create();
  if (!session.has_value()) {
    ADD_FAILURE() << "no resource compiler session";
    return {};
  }

  session->add_request({
    .session_info = {.name = "test", .root_directory = directory},
    .shaders = {{
      .path = "layout.slang",
      .module_name = "layout",
      .entry_points = {"cs_main", "pb_main", "vs_main"},
    }},
  });
  const auto is_compiled = session->compile();
  for (const auto& error : session->get_errors()) {
    ADD_FAILURE() << error;
  }

  const auto pack_path = directory / "layout.oxpack";
  const auto is_written = is_compiled && session->write_to_file(pack_path);
  session->destroy();
  if (!is_written) {
    return {};
  }

  auto pack = AssetFile::unpack(pack_path);
  if (!pack.has_value() || pack->entries.empty()) {
    ADD_FAILURE() << "pack did not unpack";
    return {};
  }

  const auto* pipeline = std::get_if<ShaderPipelineData>(&pack->entries.front().data);
  return pipeline ? pipeline->entry_points : std::vector<ShaderEntryPointData>{};
}

static auto entry(std::span<const ShaderEntryPointData> entries, std::string_view name) -> const vir::ShaderLayout* {
  const auto it = std::ranges::find(entries, name, &ShaderEntryPointData::name);
  return it == entries.end() ? nullptr : &it->layout;
}

static auto binding(const vir::ShaderLayout& layout, u32 set, u32 index) -> const vir::DescriptorBinding* {
  const auto it = std::ranges::find_if(layout.bindings, [set, index](const vir::DescriptorBinding& candidate) {
    return candidate.set == set && candidate.binding == index;
  });
  return it == layout.bindings.end() ? nullptr : &*it;
}

TEST(ShaderLayout, ComputeLayoutComesFromSlangReflection) {
  const auto entries = compile_layouts();
  const auto* layout = entry(entries, "cs_main");
  ASSERT_NE(layout, nullptr);
  EXPECT_EQ(layout->stage, vir::ShaderStage::Compute);
  EXPECT_EQ(layout->entry_point, "cs_main");
  EXPECT_EQ(layout->local_size, (std::array<u32, 3>{8, 4, 1}));
  EXPECT_EQ(layout->push_constant_offset, 0u);
  EXPECT_EQ(layout->push_constant_size, 20u);

  const auto expect = [&](u32 set, u32 index, VkDescriptorType type, vir::DescriptorUsage usage) {
    SCOPED_TRACE(index);
    const auto* found = binding(*layout, set, index);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->descriptor_type, type);
    EXPECT_EQ(found->usage, usage);
    EXPECT_EQ(found->stages, vir::ShaderStage::Compute);
  };
  expect(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vir::DescriptorUsage::Read);
  expect(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vir::DescriptorUsage::ReadWrite);
  expect(0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, vir::DescriptorUsage::Sampled);
  expect(0, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, vir::DescriptorUsage::ReadWrite);
  expect(0, 4, VK_DESCRIPTOR_TYPE_SAMPLER, vir::DescriptorUsage::None);
  expect(0, 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vir::DescriptorUsage::Uniform);
  expect(1, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, vir::DescriptorUsage::Sampled);
  EXPECT_TRUE(binding(*layout, 1, 0)->is_variable_count);
  EXPECT_EQ(binding(*layout, 0, 6), nullptr) << "a binding the entry point never reads is dropped";

  const auto spec = std::ranges::find(layout->spec_constants, 7u, &vir::SpecConstant::id);
  ASSERT_NE(spec, layout->spec_constants.end());
  EXPECT_EQ(spec->size, 4u);
}

TEST(ShaderLayout, EntryPointParameterBlocksGetTheirOwnSet) {
  const auto entries = compile_layouts();
  const auto* layout = entry(entries, "pb_main");
  ASSERT_NE(layout, nullptr);
  ASSERT_EQ(layout->bindings.size(), 1u);
  // sets 0 and 1 belong to the module's globals
  EXPECT_EQ(layout->bindings[0].set, 2u);
  EXPECT_EQ(layout->bindings[0].descriptor_type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  EXPECT_EQ(layout->bindings[0].usage, vir::DescriptorUsage::ReadWrite);
  EXPECT_GE(layout->push_constant_size, 4u);
}

TEST(ShaderLayout, VertexInputsSkipSystemValuesAndFollowLocations) {
  const auto entries = compile_layouts();
  const auto* layout = entry(entries, "vs_main");
  ASSERT_NE(layout, nullptr);
  EXPECT_EQ(layout->stage, vir::ShaderStage::Vertex);
  EXPECT_EQ(
    layout->vertex_inputs,
    (std::vector<vir::VertexInput>{
      {.location = 0, .format = vir::Format::RGB32Sfloat, .size = 12},
      {.location = 1, .format = vir::Format::RG32Sfloat, .size = 8},
      {.location = 2, .format = vir::Format::R32Uint, .size = 4},
    })
  );
}
} // namespace ox
