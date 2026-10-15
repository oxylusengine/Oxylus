#include <fmt/format.h>

#include "Render/Graph/Module.hpp"

namespace ox::vir {
struct Printer {
  const ResourceIndex* resources = nullptr;
  ankerl::unordered_dense::set<ValueID> bound = {};

  auto constant(this const Printer& self, ValueID id) -> const ir::Constant* {
    const auto* ir = self.resources->get(id);
    return ir ? std::get_if<ir::Constant>(ir) : nullptr;
  }

  auto string(this const Printer& self, ValueID id) -> std::string_view {
    const auto* found = self.constant(id);
    return found && found->kind == ir::Constant::Kind::String ? std::string_view(found->string) : std::string_view{};
  }

  auto name(this const Printer& self, ValueID id) -> std::string {
    const auto [resolution, base] = self.resources->resolve(id);
    if (!base) {
      return {};
    }

    if (std::holds_alternative<ir::SwapchainImage>(*base)) {
      return "swapchain";
    }

    if (const auto* image = std::get_if<ir::ConstructImage>(base)) {
      return std::string(self.string(image->name));
    }

    if (const auto* buffer = std::get_if<ir::ConstructBuffer>(base)) {
      return std::string(self.string(buffer->name));
    }

    return {};
  }

  auto format_constant(this const Printer&, const ir::Constant& constant) -> std::string {
    switch (constant.kind) {
      case ir::Constant::Kind::I32: return fmt::format("{}", constant.as_i32());
      case ir::Constant::Kind::U32:
        return constant.as_u32() == REMAINING ? "remaining" : fmt::format("{}", constant.as_u32());
      case ir::Constant::Kind::U64     : return fmt::format("{}", constant.as_u64());
      case ir::Constant::Kind::Extent2D: return fmt::format("{}x{}", constant.words[0], constant.words[1]);
      case ir::Constant::Kind::Extent3D:
        return fmt::format("{}x{}x{}", constant.words[0], constant.words[1], constant.words[2]);
      case ir::Constant::Kind::String    : return fmt::format("\"{}\"", constant.string);
      case ir::Constant::Kind::Access    : return to_string(constant.as_access());
      case ir::Constant::Kind::ClearValue: {
        const auto clear = constant.as_clear();
        const auto& color = clear.value.color.float32;
        return fmt::format("[{:.3f}, {:.3f}, {:.3f}, {:.3f}]", color[0], color[1], color[2], color[3]);
      }
    }

    return {};
  }

  auto operand(this const Printer& self, ValueID id) -> std::string {
    if (!is_valid(id)) {
      return "none";
    }

    if (const auto* found = self.constant(id)) {
      return fmt::format("%{}({})", std::to_underlying(id), self.format_constant(*found));
    }

    if (const auto* ir = self.resources->get(id)) {
      if (const auto* variable = std::get_if<ir::Variable>(ir)) {
        return fmt::format("%{}(${})", std::to_underlying(id), self.string(variable->name));
      }
    }

    const auto resource_name = self.name(id);
    if (resource_name.empty()) {
      return fmt::format("%{}", std::to_underlying(id));
    }

    return fmt::format("%{}({})", std::to_underlying(id), resource_name);
  }

  auto range(this const Printer&, const Subresource& range) -> std::string {
    if (range.is_whole()) {
      return {};
    }

    const auto count = [](u32 value) {
      return value == REMAINING ? std::string("*") : fmt::format("{}", value);
    };
    return fmt::format(
      " mips={}+{} layers={}+{}",
      range.base_level,
      count(range.level_count),
      range.base_layer,
      count(range.layer_count)
    );
  }

  auto line(this const Printer& self, const Instr& instruction) -> std::string {
    const auto op = [&](ValueID id) {
      return self.operand(id);
    };
    return std::visit(
      [&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ir::Type>) {
          switch (node.kind) {
            case ir::TypeKind::U64   : return "type u64";
            case ir::TypeKind::Buffer: return "type buffer";
            case ir::TypeKind::Image:
              return fmt::format(
                "type image format={} samples={}",
                std::to_underlying(node.format),
                std::to_underlying(node.samples)
              );
          }

          return "type";
        } else if constexpr (std::is_same_v<T, ir::Constant>) {
          return fmt::format("const {}", self.format_constant(node));
        } else if constexpr (std::is_same_v<T, ir::Variable>) {
          return fmt::format("var ${} {} slot={}", self.string(node.name), ir::to_string(node.kind), node.slot);
        } else if constexpr (std::is_same_v<T, ir::Array>) {
          auto elements = std::string{};
          for (const auto element : node.elements) {
            elements += (elements.empty() ? "" : ", ") + op(element);
          }

          return fmt::format("array [{}]", elements);
        } else if constexpr (std::is_same_v<T, ir::Index>) {
          return fmt::format("index {} {}", op(node.array), op(node.index));
        } else if constexpr (std::is_same_v<T, ir::ConstructBuffer>) {
          const auto kind = self.bound.contains(instruction.id) ? "bound"
                            : node.buffer.is_null()             ? "transient"
                                                                : "imported";
          return fmt::format(
            "buffer.{} {} size={} usage={:#x} rests={}",
            kind,
            op(node.name),
            op(node.size),
            node.usage,
            op(node.initial_access)
          );
        } else if constexpr (std::is_same_v<T, ir::ConstructImage>) {
          const auto kind = self.bound.contains(instruction.id) ? "bound"
                            : node.image.is_null()              ? "transient"
                                                                : "imported";
          return fmt::format(
            "image.{} {} format={} extent={} mips={} layers={} usage={:#x} rests={}",
            kind,
            op(node.name),
            std::to_underlying(node.format),
            op(node.extent),
            op(node.level_count),
            op(node.layer_count),
            std::to_underlying(node.usage),
            op(node.initial_access)
          );
        } else if constexpr (std::is_same_v<T, ir::View>) {
          return fmt::format("view {}{}", op(node.image), self.range(node.range));
        } else if constexpr (std::is_same_v<T, ir::AcquireNextImage>) {
          return fmt::format("acquire {}", op(node.swapchain));
        } else if constexpr (std::is_same_v<T, ir::SwapchainImage>) {
          return fmt::format("swapchain_image {} {}", op(node.swapchain), op(node.acquire));
        } else if constexpr (std::is_same_v<T, ir::Export>) {
          return fmt::format("export {} {}", op(node.resource), op(node.access));
        } else if constexpr (std::is_same_v<T, ir::Clear>) {
          return fmt::format("clear {} {}", op(node.attachment), op(node.color));
        } else if constexpr (std::is_same_v<T, ir::Blit>) {
          return fmt::format("blit {} -> {}", op(node.src), op(node.dst));
        } else if constexpr (std::is_same_v<T, ir::CopyBufferToImage>) {
          return fmt::format("copy {} -> {}", op(node.buffer), op(node.image));
        } else if constexpr (std::is_same_v<T, ir::CopyImageToBuffer>) {
          return fmt::format("copy {} -> {}", op(node.image), op(node.buffer));
        } else if constexpr (std::is_same_v<T, ir::CopyBuffer>) {
          return fmt::format(
            "copy {} -> {} src={} dst={} size={}",
            op(node.src),
            op(node.dst),
            node.region.src_offset,
            node.region.dst_offset,
            node.region.size
          );
        } else if constexpr (std::is_same_v<T, ir::FillBuffer>) {
          return fmt::format("fill {} with {:#x}", op(node.buffer), node.data);
        } else if constexpr (std::is_same_v<T, ir::UpdateBuffer>) {
          return fmt::format(
            "update {} offset={} source={} bytes={}",
            op(node.buffer),
            node.offset,
            op(node.source),
            node.data.size()
          );
        } else if constexpr (std::is_same_v<T, ir::CopyImage>) {
          return fmt::format("copy {} -> {}", op(node.src), op(node.dst));
        } else if constexpr (std::is_same_v<T, ir::ResolveImage>) {
          return fmt::format("resolve {} -> {}", op(node.src), op(node.dst));
        } else if constexpr (std::is_same_v<T, ir::ConstructQueryPool>) {
          return fmt::format("query_pool type={} count={}", std::to_underlying(node.type), node.count);
        } else if constexpr (std::is_same_v<T, ir::BuildAccelerationStructures>) {
          return fmt::format(
            "build_acceleration_structures {} addresses={} resources={}",
            op(node.builds),
            node.addresses.size(),
            node.resources.size()
          );
        } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
          return fmt::format(
            "write_acceleration_structure_properties {} queries={} first={}",
            op(node.source),
            op(node.queries),
            node.first_query
          );
        } else if constexpr (std::is_same_v<T, ir::CopyAccelerationStructure>) {
          return fmt::format(
            "copy_acceleration_structure {} -> {} mode={}",
            op(node.src),
            op(node.dst),
            std::to_underlying(node.mode)
          );
        } else if constexpr (std::is_same_v<T, ir::BeginRendering> || std::is_same_v<T, ir::BeginCompute>) {
          auto attachments = std::string{};
          for (const auto& [resource, access] : node.attachments) {
            attachments += fmt::format("{}{}:{}", attachments.empty() ? "" : ", ", op(resource), op(access));
          }

          constexpr auto KIND = std::is_same_v<T, ir::BeginRendering> ? "begin_rendering" : "begin_compute";
          return fmt::format("{} {} [{}]", KIND, op(node.name), attachments);
        } else if constexpr (std::is_same_v<T, ir::BindPipeline>) {
          return fmt::format("bind_pipeline #{}", std::to_underlying(node.pipeline));
        } else if constexpr (std::is_same_v<T, ir::SetState>) {
          return "set_state";
        } else if constexpr (std::is_same_v<T, ir::BindVertexBuffers>) {
          return fmt::format("bind_vertex_buffers {}", op(node.buffers));
        } else if constexpr (std::is_same_v<T, ir::BindIndexBuffer>) {
          return fmt::format("bind_index_buffer {}", op(node.buffer));
        } else if constexpr (std::is_same_v<T, ir::WriteDescriptor>) {
          const auto resource = node.descriptor.resource();
          return fmt::format(
            "descriptor set={} binding={} {} {}",
            node.set,
            node.binding,
            resource.has_value() ? op(*resource) : std::string("sampler"),
            op(node.access)
          );
        } else if constexpr (std::is_same_v<T, ir::Draw>) {
          return fmt::format(
            "draw {} {} pipeline=#{}",
            op(node.vertex_count),
            op(node.instance_count),
            std::to_underlying(node.pipeline)
          );
        } else if constexpr (std::is_same_v<T, ir::DrawIndexed>) {
          return fmt::format(
            "draw_indexed {} {} pipeline=#{}",
            op(node.index_count),
            op(node.instance_count),
            std::to_underlying(node.pipeline)
          );
        } else if constexpr (std::is_same_v<T, ir::DrawIndirect>) {
          return fmt::format("draw_indirect {} pipeline=#{}", op(node.buffer), std::to_underlying(node.pipeline));
        } else if constexpr (std::is_same_v<T, ir::DrawMeshTasks>) {
          return fmt::format(
            "draw_mesh_tasks {} {} {} pipeline=#{}",
            op(node.x),
            op(node.y),
            op(node.z),
            std::to_underlying(node.pipeline)
          );
        } else if constexpr (std::is_same_v<T, ir::DrawMeshTasksIndirect>) {
          return fmt::format(
            "draw_mesh_tasks_indirect {} pipeline=#{}",
            op(node.buffer),
            std::to_underlying(node.pipeline)
          );
        } else if constexpr (std::is_same_v<T, ir::CallOpaque>) {
          return fmt::format("call {}", op(node.body));
        } else if constexpr (std::is_same_v<T, ir::EndRendering>) {
          return "end_rendering";
        } else if constexpr (std::is_same_v<T, ir::Dispatch>) {
          return fmt::format("dispatch pipeline=#{}", std::to_underlying(node.pipeline));
        } else if constexpr (std::is_same_v<T, ir::TraceRays>) {
          return fmt::format(
            "trace_rays {} {} {} pipeline=#{}",
            op(node.width),
            op(node.height),
            op(node.depth),
            std::to_underlying(node.pipeline)
          );
        } else if constexpr (std::is_same_v<T, ir::EndCompute>) {
          return "end_compute";
        } else if constexpr (std::is_same_v<T, ir::PassResult>) {
          return fmt::format("result {}", op(node.resource));
        } else if constexpr (std::is_same_v<T, ir::Label>) {
          return fmt::format("label {}", std::to_underlying(node.label));
        } else if constexpr (std::is_same_v<T, ir::SelectionMerge>) {
          return fmt::format("selection_merge label {}", std::to_underlying(node.merge));
        } else if constexpr (std::is_same_v<T, ir::Branch>) {
          return fmt::format("branch label {}", std::to_underlying(node.target));
        } else if constexpr (std::is_same_v<T, ir::BranchConditional>) {
          return fmt::format(
            "branch_cond {} -> label {}, label {}",
            op(node.condition),
            std::to_underlying(node.true_label),
            std::to_underlying(node.false_label)
          );
        } else if constexpr (std::is_same_v<T, ir::Return>) {
          return "return";
        } else if constexpr (std::is_same_v<T, ir::Phi>) {
          auto incoming = std::string{};
          for (const auto& [value, label] : node.incoming) {
            incoming += fmt::format(
              "{}{} from label {}",
              incoming.empty() ? "" : ", ",
              op(value),
              std::to_underlying(label)
            );
          }

          return fmt::format("phi [{}]", incoming);
        } else if constexpr (std::is_same_v<T, ir::MemoryBarrier>) {
          return fmt::format("barrier.memory {} -> {}", op(node.src_access), op(node.dst_access));
        } else if constexpr (std::is_same_v<T, ir::ImageBarrier>) {
          return fmt::format(
            "barrier.image {}{} layout {} -> {} access {} -> {}",
            op(node.value),
            self.range(node.range),
            std::to_underlying(node.old_layout),
            std::to_underlying(node.new_layout),
            op(node.src_access),
            op(node.dst_access)
          );
        } else {
          return "?";
        }
      },
      instruction.ir
    );
  }
};

auto dump(std::span<const Instr> instructions, std::span<const ValueID> bound, bool is_highlighted) -> std::string {
  const auto resources = ResourceIndex(instructions);
  auto printer = Printer{.resources = &resources};
  printer.bound.insert(bound.begin(), bound.end());

  auto passes = 0_sz;
  auto barriers = 0_sz;
  for (const auto& instruction : instructions) {
    passes += opens_region(instruction.ir) ? 1 : 0;
    barriers += is_barrier(instruction.ir) ? 1 : 0;
  }

  auto out = fmt::format(
    "; render graph: {} instructions, {} passes, {} barriers\n",
    instructions.size(),
    passes,
    barriers
  );
  auto depth = 0;
  for (const auto& instruction : instructions) {
    if (closes_region(instruction.ir)) {
      depth -= 1;
    }

    const auto text = printer.line(instruction);
    const auto is_barrier_line = is_barrier(instruction.ir);
    const auto* prefix = is_highlighted && is_barrier_line ? "\x1b[33m" : "";
    const auto* suffix = is_highlighted && is_barrier_line ? "\x1b[0m" : "";
    out += fmt::format(
      "{:>6} = {}{}{}{}\n",
      fmt::format("%{}", std::to_underlying(instruction.id)),
      std::string(static_cast<usize>(std::max(depth, 0)) * 2, ' '),
      prefix,
      text,
      suffix
    );

    if (opens_region(instruction.ir)) {
      depth += 1;
    }
  }

  return out;
}

auto Program::dump(this const Program& self, bool is_highlighted) -> std::string {
  auto bound = std::vector<ValueID>{};
  for (const auto& [resource, slot] : self.bound) {
    bound.push_back(resource);
  }

  return vir::dump(self.instructions, bound, is_highlighted);
}
} // namespace ox::vir
