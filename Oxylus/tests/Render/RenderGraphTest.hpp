#pragma once

#include <gtest/gtest.h>

#include "Render/Graph/Module.hpp"
#include "Render/Graph/Types.hpp"

namespace ox {
using namespace vir;

constexpr auto FORMAT = vir::Format::RGBA8Srgb;
constexpr auto DEPTH_FORMAT = vir::Format::D32Sfloat;
constexpr u32 WIDTH = 64;
constexpr u32 HEIGHT = 32;

inline auto extent_2d() -> vir::Extent2D { return {WIDTH, HEIGHT}; }

inline auto transient_info() -> ImageInfo { return ImageInfo::color_target(extent_2d(), FORMAT); }

// an image declaring no usage, so what it is created with is what the graph inferred
inline auto untyped_info() -> ImageInfo { return {.extent = {WIDTH, HEIGHT, 1}, .format = FORMAT}; }

inline auto a_sampler() -> VkSampler { return reinterpret_cast<VkSampler>(1_u64); }

inline auto module_with_attachment(Module& module) -> ValueID {
  const auto image = Image::imported(VK_NULL_HANDLE, FORMAT, {WIDTH, HEIGHT, 1}, vir::Samples::e1);
  auto attachment = ImageAttachment::from_image(image, VK_IMAGE_LAYOUT_UNDEFINED);
  const auto construct = module.import_attachment(attachment, Access::None);
  module.set_name(construct, "target");
  return construct;
}

// bindings written out by a test instead of reflected from shaders
struct Declared {
  std::vector<DescriptorBinding> bindings = {};
  option<u32> bindless = nullopt;

  static auto with_access(
    std::initializer_list<std::tuple<u32, u32, VkDescriptorType>> declared, vir::ShaderStage stages, Access access
  ) -> Declared {
    auto result = Declared{};
    for (const auto& [set, binding, type] : declared) {
      result.bindings.push_back({
        .set = set,
        .binding = binding,
        .descriptor_type = type,
        .count = 1,
        .stages = stages,
        .access = access,
      });
    }

    return result;
  }

  static auto fragment(std::initializer_list<std::tuple<u32, u32, VkDescriptorType>> declared) -> Declared {
    return with_access(declared, vir::ShaderStage::Fragment, sampled_by(vir::ShaderStage::Fragment));
  }

  auto pipelines(this const Declared& self) -> PipelineBindings {
    return {
      .user = &self,
      .bindings_fn = [](const void* user, PipelineID) -> option<std::span<const DescriptorBinding>> {
        return std::span<const DescriptorBinding>(static_cast<const Declared*>(user)->bindings);
      },
      .bindless_set_fn = [](const void* user, PipelineID) -> option<u32> {
        return static_cast<const Declared*>(user)->bindless;
      },
    };
  }
};

inline auto access_of(const Module& module, const Program& program, ValueID id) -> Access {
  for (const auto& instruction : program.instructions) {
    if (instruction.id == id) {
      return std::get<ir::Constant>(instruction.ir).as_access();
    }
  }

  return std::get<ir::Constant>(module.get(id)).as_access();
}

struct Barrier {
  Access src = Access::Empty;
  Access dst = Access::Empty;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  ValueID resource = ValueID::Invalid;
  Subresource range = {};
};

template <typename T>
inline auto barrier_entries(const IR& instruction) -> std::span<const T> {
  if (const auto* barrier = std::get_if<T>(&instruction)) {
    return std::span(barrier, 1);
  }
  return {};
}

inline auto image_barriers(const Module& module, const Program& program) -> std::vector<Barrier> {
  auto barriers = std::vector<Barrier>{};
  for (const auto& instruction : program.instructions) {
    if (const auto* barrier = std::get_if<ir::ImageBarrier>(&instruction.ir)) {
      barriers.push_back({
        .src = access_of(module, program, barrier->src_access),
        .dst = access_of(module, program, barrier->dst_access),
        .old_layout = barrier->old_layout,
        .new_layout = barrier->new_layout,
        .resource = module.resource_root(barrier->value),
        .range = barrier->range,
      });
    }
  }

  return barriers;
}

inline auto memory_barriers(const Module& module, const Program& program) -> std::vector<std::pair<Access, Access>> {
  auto barriers = std::vector<std::pair<Access, Access>>{};
  for (const auto& instruction : program.instructions) {
    if (const auto* barrier = std::get_if<ir::MemoryBarrier>(&instruction.ir)) {
      barriers.emplace_back(
        access_of(module, program, barrier->src_access),
        access_of(module, program, barrier->dst_access)
      );
    }
  }

  return barriers;
}

template <typename T>
inline auto find_all(const Program& program) -> std::vector<const T*> {
  auto found = std::vector<const T*>{};
  for (const auto& instruction : program.instructions) {
    if (const auto* node = std::get_if<T>(&instruction.ir)) {
      found.push_back(node);
    }
  }

  return found;
}

template <typename T>
inline auto find_all(const Program&&) -> std::vector<const T*> = delete;

template <typename T>
inline auto position_of(const Program& program) -> usize {
  for (usize index = 0; index < program.instructions.size(); index++) {
    if (std::holds_alternative<T>(program.instructions[index].ir)) {
      return index;
    }
  }

  return ~0_sz;
}

inline auto image_usage(const Program& program, ValueID id) -> vir::ImageUsageFlags {
  for (const auto& instruction : program.instructions) {
    if (instruction.id == id) {
      return std::get<ir::ConstructImage>(instruction.ir).usage;
    }
  }

  ADD_FAILURE() << "the image should still be declared after compiling";
  return vir::ImageUsageFlags::None;
}

inline auto buffer_usage(const Program& program, ValueID id) -> VkBufferUsageFlags {
  for (const auto& instruction : program.instructions) {
    if (instruction.id == id) {
      return std::get<ir::ConstructBuffer>(instruction.ir).usage;
    }
  }

  ADD_FAILURE() << "the buffer should still be declared after compiling";
  return 0;
}

inline auto compile(const Module& module, ValueID root) -> Program {
  auto compiled = module.compile(PipelineBindings::unchecked(), root);
  EXPECT_TRUE(compiled.has_value());
  return compiled.has_value() ? std::move(*compiled) : Program{};
}

inline auto pipeline(u32 index) -> PipelineID { return static_cast<PipelineID>(index); }

} // namespace ox
