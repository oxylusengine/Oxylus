#include <map>

#include "Render/Graph/Module.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
struct WrittenDescriptor {
  ValueID written_at = ValueID::Invalid;
  ir::Descriptor descriptor = {};
  option<VkDescriptorType> resolved_type = nullopt;
};

static auto names_resource(const ResourceIndex& resources, ValueID id, bool is_image) -> bool {
  const auto [resolution, base] = resources.resolve(id);
  if (!base) {
    return false;
  }

  if (is_image) {
    return std::holds_alternative<ir::ConstructImage>(*base) || std::holds_alternative<ir::SwapchainImage>(*base);
  }

  return std::holds_alternative<ir::ConstructBuffer>(*base);
}

static auto write_is_sound(
  const ResourceIndex& resources, ValueID value_id, bool is_open, const ir::WriteDescriptor& write
) -> bool {
  const auto set = write.set;
  const auto binding = write.binding;
  const auto& descriptor = write.descriptor;
  if (!is_open) {
    OX_LOG_ERROR(
      "descriptor %{} at set {} binding {} is written outside of any pass",
      std::to_underlying(value_id),
      set,
      binding
    );
    return false;
  }

  if (const auto image = descriptor.image_value(); image.has_value() && !names_resource(resources, *image, true)) {
    OX_LOG_ERROR("image descriptor at set {} binding {} names a value that is not an image", set, binding);
    return false;
  }

  if (const auto buffer = descriptor.buffer_value(); buffer.has_value() && !names_resource(resources, *buffer, false)) {
    OX_LOG_ERROR("buffer descriptor at set {} binding {} names a value that is not a buffer", set, binding);
    return false;
  }

  const auto has_sampler = descriptor.kind == ir::Descriptor::Kind::Sampler ||
                           descriptor.kind == ir::Descriptor::Kind::CombinedImageSampler;
  if (has_sampler && descriptor.sampler == VK_NULL_HANDLE) {
    OX_LOG_ERROR("sampler descriptor at set {} binding {} is written with a null sampler", set, binding);
    return false;
  }

  if (descriptor.kind == ir::Descriptor::Kind::TexelBuffer && descriptor.view == VK_NULL_HANDLE) {
    OX_LOG_ERROR("texel buffer descriptor at set {} binding {} is written with a null view", set, binding);
    return false;
  }

  if (
    descriptor.kind == ir::Descriptor::Kind::Buffer &&
    (descriptor.range == 0 ||
     (descriptor.range != VK_WHOLE_SIZE && descriptor.offset + descriptor.range < descriptor.offset))
  ) {
    OX_LOG_ERROR("buffer descriptor at set {} binding {} has an invalid range", set, binding);
    return false;
  }

  if (
    descriptor.kind == ir::Descriptor::Kind::AccelerationStructure &&
    descriptor.acceleration_structure == VK_NULL_HANDLE
  ) {
    OX_LOG_ERROR("acceleration structure descriptor at set {} binding {} is written with a null handle", set, binding);
    return false;
  }

  return true;
}

static auto descriptors_are_compatible(
  ValueID value_id,
  const std::map<std::pair<u32, u32>, WrittenDescriptor>& written,
  PipelineID pipeline,
  const PipelineBindings& pipelines
) -> bool {
  // the inference already warned about it, there is nothing to check the writes against
  if (!is_valid(pipeline)) {
    return true;
  }

  const auto bindings = pipelines.bindings(pipeline);
  if (!bindings.has_value()) {
    return true;
  }

  const auto bindless = pipelines.bindless_set(pipeline);
  auto is_sound = true;
  for (const auto& binding : *bindings) {
    if (bindless == binding.set) {
      continue;
    }

    const auto it = written.find({binding.set, binding.binding});
    if (it == written.end()) {
      OX_LOG_ERROR(
        "%{} runs pipeline {} which declares set {} binding {}, but nothing has been written there",
        std::to_underlying(value_id),
        std::to_underlying(pipeline),
        binding.set,
        binding.binding
      );
      is_sound = false;
      continue;
    }

    const auto& [written_at, descriptor, resolved_type] = it->second;
    if (!descriptor.supports_type(binding.descriptor_type)) {
      OX_LOG_ERROR(
        "descriptor written at %{} is incompatible with the type pipeline {} declares at set {} binding {}",
        std::to_underlying(written_at),
        std::to_underlying(pipeline),
        binding.set,
        binding.binding
      );
      is_sound = false;
    } else if (resolved_type != binding.descriptor_type) {
      OX_LOG_ERROR(
        "descriptor written at %{} was not resolved to the type pipeline {} declares at set {} binding {}",
        std::to_underlying(written_at),
        std::to_underlying(pipeline),
        binding.set,
        binding.binding
      );
      is_sound = false;
    }
  }

  // a bindless set is handed to the pipeline whole, writing into it goes to a set the graph doesn't own
  for (const auto& [key, write] : written) {
    if (bindless == key.first) {
      OX_LOG_ERROR(
        "a descriptor is written into the bindless set {} of pipeline {}",
        key.first,
        std::to_underlying(pipeline)
      );
      is_sound = false;
    }
  }

  return is_sound;
}

auto analyze_descriptors(const Program& program, const PipelineBindings& pipelines) -> std::expected<void, VkResult> {
  const auto resources = ResourceIndex(program.instructions);
  auto written = std::map<std::pair<u32, u32>, WrittenDescriptor>{};
  auto is_open = false;
  auto is_failed = false;

  for (const auto& instruction : program.instructions) {
    const auto& ir = instruction.ir;
    if (opens_region(ir)) {
      is_open = true;
      written.clear();
    } else if (closes_region(ir)) {
      is_open = false;
      written.clear();
    } else if (const auto* write = std::get_if<ir::WriteDescriptor>(&ir)) {
      is_failed |= !write_is_sound(resources, instruction.id, is_open, *write);
      written.insert_or_assign(
        std::pair{write->set, write->binding},
        WrittenDescriptor{
          .written_at = instruction.id,
          .descriptor = write->descriptor,
          .resolved_type = write->descriptor_type
        }
      );
    } else if (std::holds_alternative<ir::CallOpaque>(ir)) {
      // a callback binds whatever it draws with itself
      continue;
    } else if (const auto* pipeline = pipeline_of(ir)) {
      is_failed |= !descriptors_are_compatible(instruction.id, written, *pipeline, pipelines);
    }
  }

  if (is_failed) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  return {};
}
} // namespace ox::vir
