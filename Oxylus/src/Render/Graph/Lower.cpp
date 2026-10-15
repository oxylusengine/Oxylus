#include <bit>
#include <map>

#include "Render/Graph/Module.hpp"
#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
static auto alloc_id(u32& next_id) -> ValueID {
  OX_ASSERT(next_id != ~0_u32, "exhausted valid value ids");
  return static_cast<ValueID>(next_id++);
}

static auto is_global(const IR& ir) -> bool {
  return std::holds_alternative<ir::Type>(ir) || std::holds_alternative<ir::Constant>(ir) ||
         std::holds_alternative<ir::Variable>(ir) || std::holds_alternative<ir::Array>(ir) ||
         std::holds_alternative<ir::ConstructBuffer>(ir) || std::holds_alternative<ir::ConstructImage>(ir) ||
         std::holds_alternative<ir::ConstructQueryPool>(ir);
}

auto globals_first(std::vector<Instr> nodes) -> std::vector<Instr> {
  auto result = std::vector<Instr>{};
  result.reserve(nodes.size());
  for (auto& node : nodes) {
    if (std::holds_alternative<ir::Type>(node.ir)) {
      result.push_back(std::move(node));
    }
  }

  for (auto& node : nodes) {
    if (std::holds_alternative<ir::Constant>(node.ir)) {
      result.push_back(std::move(node));
    }
  }

  for (auto& node : nodes) {
    if (!std::holds_alternative<ir::Type>(node.ir) && !std::holds_alternative<ir::Constant>(node.ir)) {
      result.push_back(std::move(node));
    }
  }

  return result;
}

auto Module::compile(this const Module& self, const PipelineBindings& pipelines, ValueID root)
  -> std::expected<Program, VkResult> {
  const ValueID roots[] = {root};
  return self.compile_all(pipelines, roots);
}

auto Module::compile_all(this const Module& self, const PipelineBindings& pipelines, std::span<const ValueID> roots)
  -> std::expected<Program, VkResult> {
  ZoneScoped;

  OX_ASSERT(!self.ongoing_pass.has_value(), "a module cannot compile with a pass still open");
  auto nodes = self.lower(pipelines, roots);
  if (!nodes) {
    return std::unexpected(nodes.error());
  }

  auto slots = ankerl::unordered_dense::map<ValueID, u32>{};
  for (u32 slot = 0; slot < self.variable_ids.size(); slot++) {
    slots.emplace(self.variable_ids[slot], slot);
  }

  auto program = Program(std::move(*nodes), self.variables, std::move(slots));
  if (auto analyzed = analyze_descriptors(program, pipelines); !analyzed) {
    return std::unexpected(analyzed.error());
  }

  return program;
}

auto Module::lower(this const Module& self, const PipelineBindings& pipelines, std::span<const ValueID> ids)
  -> std::expected<std::vector<Instr>, VkResult> {
  auto roots = std::vector<ValueID>(ids.begin(), ids.end());
  roots.insert(roots.end(), self.kept.begin(), self.kept.end());
  for (const auto& block : self.blocks) {
    if (block.terminator.kind == TerminatorKind::BranchConditional) {
      roots.push_back(block.terminator.condition);
    }
  }

  for (usize index = 0; index < self.instructions.size(); index++) {
    if (std::holds_alternative<ir::Type>(self.instructions[index])) {
      roots.push_back(static_cast<ValueID>(index));
    }
  }

  auto next_id = static_cast<u32>(self.instructions.size());
  auto nodes = self.topo_sort(roots);
  nodes = self.layout_blocks(std::move(nodes), next_id);
  nodes = self.infer(std::move(nodes));
  // reflected accesses drive barriers and reflected types drive usage, so both resolve first
  auto resolved = resolve_descriptors(std::move(nodes), pipelines, next_id);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  nodes = self.fold_clears(std::move(*resolved));
  nodes = self.sync(std::move(nodes), next_id);
  nodes = simplify_cfg(std::move(nodes));
  nodes = globals_first(std::move(nodes));
  nodes = self.schedule_barriers(std::move(nodes));
  nodes = fold_barriers(std::move(nodes), next_id);
  nodes = globals_first(std::move(nodes));
  self.infer_usage(nodes);
  return normalize_barriers(std::move(nodes));
}

auto Module::topo_sort(this const Module& self, std::span<const ValueID> roots) -> std::vector<Instr> {
  auto nodes = std::vector<Instr>{};
  auto stack = std::vector<ValueID>(roots.rbegin(), roots.rend());
  auto visited = std::vector<bool>(self.instructions.size(), false);
  auto processed = std::vector<bool>(self.instructions.size(), false);
  auto operands = std::vector<ValueID>{};

  while (!stack.empty()) {
    const auto id = stack.back();
    stack.pop_back();
    const auto index = index_of(id);
    if (processed[index]) {
      continue;
    }

    if (!visited[index]) {
      visited[index] = true;
      stack.push_back(id);
      operands.clear();
      visit_operands(self.instructions[index], [&](ValueID operand) { operands.push_back(operand); });
      stack.insert(stack.end(), operands.rbegin(), operands.rend());
      continue;
    }

    processed[index] = true;
    nodes.push_back({.id = id, .ir = self.instructions[index]});
  }

  return nodes;
}

auto Module::layout_blocks(this const Module& self, std::vector<Instr> nodes, u32& next_id) -> std::vector<Instr> {
  auto result = std::vector<Instr>{};
  result.reserve(nodes.size() + self.blocks.size() * 3);
  auto recorded = ankerl::unordered_dense::map<LabelID, std::vector<Instr>>{};
  for (auto& node : nodes) {
    const auto block = self.block_of(node.id);
    if (is_global(node.ir) || block == LabelID::Invalid) {
      result.push_back(std::move(node));
    } else {
      recorded[block].push_back(std::move(node));
    }
  }

  for (const auto& block : self.blocks) {
    result.push_back({.id = alloc_id(next_id), .ir = ir::Label{.label = block.label}});
    if (auto it = recorded.find(block.label); it != recorded.end()) {
      for (auto& node : it->second) {
        result.push_back(std::move(node));
      }
    }

    const auto& terminator = block.terminator;
    switch (terminator.kind) {
      case TerminatorKind::Return: result.push_back({.id = alloc_id(next_id), .ir = ir::Return{}}); break;
      case TerminatorKind::Branch:
        result.push_back({.id = alloc_id(next_id), .ir = ir::Branch{.target = terminator.target}});
        break;
      case TerminatorKind::BranchConditional:
        result.push_back({.id = alloc_id(next_id), .ir = ir::SelectionMerge{.merge = terminator.merge}});
        result.push_back({
          .id = alloc_id(next_id),
          .ir = ir::BranchConditional{
            .condition = terminator.condition,
            .true_label = terminator.true_label,
            .false_label = terminator.false_label,
          },
        });
        break;
    }
  }

  return result;
}

auto Module::infer(this const Module& self, std::vector<Instr> nodes) -> std::vector<Instr> {
  struct InForce {
    PassState state = {};
    VkRect2D area = {};
    option<PipelineID> pipeline = nullopt;
    bool is_late_area = false;
  };

  auto regions = ankerl::unordered_dense::map<ValueID, InForce>{};
  // every instruction of a pass mapped to the begin that opened it
  auto region_of = ankerl::unordered_dense::map<ValueID, ValueID>{};
  const auto region = [&](ValueID pass) -> InForce* {
    const auto begin = region_of.find(pass);
    if (begin == region_of.end()) {
      return nullptr;
    }

    const auto it = regions.find(begin->second);
    return it == regions.end() ? nullptr : &it->second;
  };
  const auto join = [&](ValueID id, ValueID pass) -> InForce* {
    const auto begin = region_of.find(pass);
    if (begin == region_of.end()) {
      return nullptr;
    }

    region_of.emplace(id, begin->second);
    return region(id);
  };

  for (auto& node : nodes) {
    std::visit(
      [&](auto& ir_node) {
        using T = std::decay_t<decltype(ir_node)>;
        if constexpr (std::is_same_v<T, ir::BeginRendering>) {
          auto rendering = RenderingState{.view_mask = ir_node.view_mask};
          auto attachment_extent = option<Extent2D>{};
          auto has_color_samples = false;
          auto color_attachments = std::vector<ValueID>{};
          auto depth_attachment = option<ValueID>{};
          for (const auto& [resource, access_id] : ir_node.attachments) {
            const auto access = self.resolve_access(access_id);
            if (intersects(access, Access::ColorRW)) {
              const auto elements = self.resource_elements(resource);
              color_attachments.insert(color_attachments.end(), elements.begin(), elements.end());
            }

            if (!depth_attachment.has_value() && intersects(access, Access::DepthStencilRW)) {
              const auto elements = self.resource_elements(resource);
              if (!elements.empty()) {
                depth_attachment = elements.front();
              }
            }
          }

          for (usize index = 0; index < color_attachments.size(); index++) {
            const auto attachment = color_attachments[index];
            const auto image = self.image_format(attachment);
            if (!image.has_value()) {
              OX_LOG_WARN(
                "cannot infer the format of attachment %{}, pipelines may not match",
                std::to_underlying(attachment)
              );
              continue;
            }

            if (index == 0) {
              rendering.samples = image->second;
              has_color_samples = true;
              if (const auto extent = self.image_extent(attachment); extent.has_value()) {
                attachment_extent = Extent2D{extent->width, extent->height};
              }
            }

            rendering.color_formats.push_back(image->first);
          }

          if (depth_attachment.has_value()) {
            if (const auto image = self.image_format(*depth_attachment); image.has_value()) {
              const auto aspects = aspect_mask(image->first);
              if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
                rendering.depth_format = image->first;
              }

              if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
                rendering.stencil_format = image->first;
              }

              // a depth-only region has nothing else to take these from
              if (!has_color_samples) {
                rendering.samples = image->second;
                if (const auto extent = self.image_extent(*depth_attachment); extent.has_value()) {
                  attachment_extent = Extent2D{extent->width, extent->height};
                }
              }
            }
          }

          const auto extent = is_valid(ir_node.render_area) ? self.extent_2d(ir_node.render_area) : attachment_extent;
          region_of.emplace(node.id, node.id);
          regions.emplace(
            node.id,
            InForce{
              .state = PassState::for_rendering(std::move(rendering)),
              .area = {.offset = {}, .extent = std::bit_cast<VkExtent2D>(extent.value_or(Extent2D{}))},
              .is_late_area = !extent.has_value(),
            }
          );
        } else if constexpr (std::is_same_v<T, ir::BeginCompute>) {
          region_of.emplace(node.id, node.id);
          regions.emplace(node.id, InForce{});
        } else if constexpr (std::is_same_v<T, ir::BindPipeline>) {
          if (auto* in_force = join(node.id, ir_node.pass)) {
            in_force->pipeline = ir_node.pipeline;
          }
        } else if constexpr (std::is_same_v<T, ir::SetState>) {
          if (auto* in_force = join(node.id, ir_node.pass)) {
            in_force->state.apply(ir_node.change);
          }
        } else if constexpr (
          std::is_same_v<T, ir::Draw> || std::is_same_v<T, ir::DrawIndexed> || std::is_same_v<T, ir::DrawIndirect> ||
          std::is_same_v<T, ir::DrawMeshTasks> || std::is_same_v<T, ir::DrawMeshTasksIndirect> ||
          std::is_same_v<T, ir::CallOpaque>
        ) {
          if (auto* in_force = join(node.id, ir_node.pass)) {
            if (!in_force->pipeline.has_value() && !std::is_same_v<T, ir::CallOpaque>) {
              OX_LOG_WARN("draw %{} has no pipeline bound", std::to_underlying(node.id));
            }

            ir_node.pipeline = in_force->pipeline.value_or(PipelineID::Invalid);
            // an area only known when the module runs cannot be baked into a pipeline
            if (in_force->is_late_area) {
              auto late = in_force->state;
              late.dynamic |= DynamicStateFlags::Viewport | DynamicStateFlags::Scissor;
              std::tie(ir_node.state, ir_node.dynamic) = late.resolve(in_force->area);
            } else {
              std::tie(ir_node.state, ir_node.dynamic) = in_force->state.resolve(in_force->area);
            }
          }
        } else if constexpr (std::is_same_v<T, ir::Dispatch> || std::is_same_v<T, ir::TraceRays>) {
          if (auto* in_force = join(node.id, ir_node.pass)) {
            if (!in_force->pipeline.has_value()) {
              OX_LOG_WARN("dispatch %{} has no pipeline bound", std::to_underlying(node.id));
            }

            ir_node.pipeline = in_force->pipeline.value_or(PipelineID::Invalid);
            ir_node.push_constants = in_force->state.push_constants;
            ir_node.specialization = in_force->state.specialization;
          }
        } else if constexpr (
          std::is_same_v<T, ir::BindVertexBuffers> || std::is_same_v<T, ir::BindIndexBuffer> ||
          std::is_same_v<T, ir::WriteDescriptor> || std::is_same_v<T, ir::EndRendering> ||
          std::is_same_v<T, ir::EndCompute>
        ) {
          join(node.id, ir_node.pass);
        }
      },
      node.ir
    );
  }

  return nodes;
}

auto resolve_descriptors(std::vector<Instr> nodes, const PipelineBindings& pipelines, u32& next_id)
  -> std::expected<std::vector<Instr>, VkResult> {
  auto written = std::map<std::pair<u32, u32>, std::pair<usize, ir::Descriptor>>{};
  auto resolved_accesses = std::map<usize, Access>{};
  auto resolved_types = std::map<usize, VkDescriptorType>{};
  auto is_open = false;
  auto is_failed = false;

  for (usize index = 0; index < nodes.size(); index++) {
    const auto& node = nodes[index];
    if (opens_region(node.ir)) {
      written.clear();
      is_open = true;
      continue;
    }

    if (closes_region(node.ir)) {
      is_open = false;
      continue;
    }

    if (const auto* write = std::get_if<ir::WriteDescriptor>(&node.ir)) {
      if (!is_open) {
        OX_LOG_ERROR("descriptor %{} is written outside of any pass", std::to_underlying(node.id));
        is_failed = true;
        continue;
      }

      written.insert_or_assign({write->set, write->binding}, std::pair{index, write->descriptor});
      resolved_accesses.insert_or_assign(index, Access::None);
      continue;
    }

    const auto* pipeline = pipeline_of(node.ir);
    if (!pipeline || written.empty()) {
      continue;
    }

    if (!is_valid(*pipeline)) {
      OX_LOG_ERROR("descriptors are consumed by %{} before a pipeline is bound", std::to_underlying(node.id));
      is_failed = true;
      continue;
    }

    const auto bindings = pipelines.bindings(*pipeline);
    if (!bindings.has_value()) {
      OX_LOG_ERROR(
        "pipeline {} has no reflection for the descriptors written before it",
        std::to_underlying(*pipeline)
      );
      is_failed = true;
      continue;
    }

    for (const auto& binding : *bindings) {
      const auto it = written.find({binding.set, binding.binding});
      if (it == written.end()) {
        continue;
      }

      const auto& [write_index, descriptor] = it->second;
      if (!descriptor.supports_type(binding.descriptor_type)) {
        OX_LOG_ERROR(
          "descriptor at set {} binding {} cannot be written as type {} the pipeline declares",
          binding.set,
          binding.binding,
          std::to_underlying(binding.descriptor_type)
        );
        is_failed = true;
        continue;
      }

      const auto [type, is_inserted] = resolved_types.emplace(write_index, binding.descriptor_type);
      if (!is_inserted && type->second != binding.descriptor_type) {
        OX_LOG_ERROR(
          "descriptor at set {} binding {} is consumed as two different types, rebind it between pipelines",
          binding.set,
          binding.binding
        );
        is_failed = true;
        continue;
      }

      auto& access = resolved_accesses[write_index];
      const auto combined = difference(access, Access::None) | difference(binding.access, Access::None);
      access = is_empty(combined) ? Access::None : combined;
    }
  }

  if (is_failed) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto access_ids = ankerl::unordered_dense::map<Access, ValueID>{};
  for (const auto& node : nodes) {
    if (
      const auto* constant = std::get_if<ir::Constant>(&node.ir);
      constant && constant->kind == ir::Constant::Kind::Access
    ) {
      access_ids.emplace(constant->as_access(), node.id);
    }
  }

  auto constants = std::vector<Instr>{};
  for (const auto& [index, access] : resolved_accesses) {
    auto it = access_ids.find(access);
    if (it == access_ids.end()) {
      const auto id = alloc_id(next_id);
      constants.push_back({.id = id, .ir = ir::Constant::from_access(access)});
      it = access_ids.emplace(access, id).first;
    }

    auto& write = std::get<ir::WriteDescriptor>(nodes[index].ir);
    write.access = it->second;
    if (const auto type = resolved_types.find(index); type != resolved_types.end()) {
      write.descriptor_type = type->second;
    }
  }

  constants.insert(constants.end(), std::make_move_iterator(nodes.begin()), std::make_move_iterator(nodes.end()));
  return constants;
}

auto Module::infer_usage(this const Module& self, std::vector<Instr>& nodes) -> void {
  auto accesses = ankerl::unordered_dense::map<ValueID, Access>{};
  for (const auto& node : nodes) {
    if (
      const auto* constant = std::get_if<ir::Constant>(&node.ir);
      constant && constant->kind == ir::Constant::Kind::Access
    ) {
      accesses.emplace(node.id, constant->as_access());
    }
  }

  const auto access_of = [&](ValueID id) -> option<Access> {
    if (const auto it = accesses.find(id); it != accesses.end()) {
      return it->second;
    }

    if (index_of(id) < self.instructions.size()) {
      if (
        const auto* constant = std::get_if<ir::Constant>(&self.get(id));
        constant && constant->kind == ir::Constant::Kind::Access
      ) {
        return constant->as_access();
      }
    }

    return nullopt;
  };

  auto image_usages = ankerl::unordered_dense::map<ValueID, ImageUsageFlags>{};
  auto buffer_usages = ankerl::unordered_dense::map<ValueID, VkBufferUsageFlags>{};
  const auto use_buffer = [&](ValueID buffer, VkBufferUsageFlags usage) {
    buffer_usages[self.resource_root(buffer)] |= usage;
  };

  for (const auto& node : nodes) {
    std::visit(
      [&](const auto& ir_node) {
        using T = std::decay_t<decltype(ir_node)>;
        if constexpr (std::is_same_v<T, ir::ImageBarrier>) {
          if (const auto access = access_of(ir_node.dst_access); access.has_value()) {
            image_usages[self.resource_root(ir_node.value)] |= to_image_usage(*access);
          }
        } else if constexpr (std::is_same_v<T, ir::WriteDescriptor>) {
          if (!ir_node.descriptor_type.has_value()) {
            return;
          }

          if (const auto image = ir_node.descriptor.image_value(); image.has_value()) {
            image_usages[self.resource_root(*image)] |= ir_node.descriptor.image_usage(*ir_node.descriptor_type);
          }

          if (const auto buffer = ir_node.descriptor.buffer_value(); buffer.has_value()) {
            use_buffer(*buffer, ir_node.descriptor.buffer_usage(*ir_node.descriptor_type));
          }
        } else if constexpr (std::is_same_v<T, ir::CopyBufferToImage>) {
          use_buffer(ir_node.buffer, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        } else if constexpr (std::is_same_v<T, ir::CopyImageToBuffer>) {
          use_buffer(ir_node.buffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        } else if constexpr (std::is_same_v<T, ir::CopyBuffer>) {
          use_buffer(ir_node.src, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
          use_buffer(ir_node.dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        } else if constexpr (std::is_same_v<T, ir::FillBuffer> || std::is_same_v<T, ir::UpdateBuffer>) {
          use_buffer(ir_node.buffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        } else if constexpr (std::is_same_v<T, ir::BuildAccelerationStructures>) {
          for (const auto& [resource, access_id] : ir_node.resources) {
            if (const auto access = access_of(access_id); access.has_value()) {
              use_buffer(resource, to_buffer_usage(*access));
            }
          }
          for (const auto& address : ir_node.addresses) {
            use_buffer(address.buffer, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
          }
        } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
          use_buffer(ir_node.source, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        } else if constexpr (std::is_same_v<T, ir::CopyAccelerationStructure>) {
          use_buffer(ir_node.src, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
          use_buffer(ir_node.dst, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        } else if constexpr (std::is_same_v<T, ir::BindVertexBuffers>) {
          for (const auto buffer : self.array_elements(ir_node.buffers)) {
            use_buffer(buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
          }
        } else if constexpr (std::is_same_v<T, ir::BindIndexBuffer>) {
          use_buffer(ir_node.buffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        } else if constexpr (std::is_same_v<T, ir::BeginRendering> || std::is_same_v<T, ir::BeginCompute>) {
          for (const auto& [resource, access_id] : ir_node.attachments) {
            const auto access = access_of(access_id);
            for (const auto element : self.resource_elements(resource)) {
              if (access.has_value() && self.is_buffer(element)) {
                use_buffer(element, to_buffer_usage(*access));
              }
            }
          }
        } else if constexpr (std::is_same_v<T, ir::Dispatch>) {
          if (ir_node.size.kind == ir::DispatchSize::Kind::Indirect) {
            use_buffer(ir_node.size.resource, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
          }
        } else if constexpr (std::is_same_v<T, ir::DrawIndirect> || std::is_same_v<T, ir::DrawMeshTasksIndirect>) {
          use_buffer(ir_node.buffer, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
          if (const auto counts = ir_node.count.count_buffer(); counts.has_value()) {
            use_buffer(*counts, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
          }
        } else if constexpr (std::is_same_v<T, ir::Export>) {
          if (const auto access = access_of(ir_node.access); access.has_value()) {
            for (const auto element : self.resource_elements(ir_node.resource)) {
              if (self.is_buffer(element)) {
                use_buffer(element, to_buffer_usage(*access));
              }
            }
          }
        } else if constexpr (std::is_same_v<T, ir::SetState>) {
          // a pushed address is the only sign a shader reaches the buffer through a pointer
          if (const auto* address = std::get_if<state::PushAddress>(&ir_node.change)) {
            use_buffer(address->buffer, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
          }
        }
      },
      node.ir
    );
  }

  // a bound or imported resource isn't created here, what it is used for is what the caller had
  // to allocate it for
  for (auto& node : nodes) {
    if (auto* image = std::get_if<ir::ConstructImage>(&node.ir); image && image->image.is_null()) {
      if (const auto it = image_usages.find(node.id); it != image_usages.end()) {
        image->usage |= it->second;
      }
    } else if (auto* buffer = std::get_if<ir::ConstructBuffer>(&node.ir); buffer && buffer->buffer.is_null()) {
      if (const auto it = buffer_usages.find(node.id); it != buffer_usages.end()) {
        buffer->usage |= it->second;
      }
    }
  }
}
} // namespace ox::vir
