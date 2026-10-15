#include <algorithm>
#include <cstddef>

#include "Render/Graph/Module.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
static auto merge_barrier_ranges(Subresource& first, const Subresource& second) -> bool {
  ZoneScoped;
  const auto same_layers = first.base_layer == second.base_layer && first.layer_count == second.layer_count;
  const auto same_levels = first.base_level == second.base_level && first.level_count == second.level_count;
  const auto combine = [](u32& base, u32& count, u32 other_base, u64 end, u64 other_end) -> void {
    base = std::min(base, other_base);
    const auto combined_end = std::max(end, other_end);
    count = combined_end == REMAINING ? REMAINING : static_cast<u32>(combined_end - base);
  };
  if (same_layers && (first.level_end() == second.base_level || second.level_end() == first.base_level)) {
    combine(first.base_level, first.level_count, second.base_level, first.level_end(), second.level_end());
    return true;
  }
  if (same_levels && (first.layer_end() == second.base_layer || second.layer_end() == first.base_layer)) {
    combine(first.base_layer, first.layer_count, second.base_layer, first.layer_end(), second.layer_end());
    return true;
  }
  return false;
}

struct BlockAddress {
  LabelID label = LabelID::Invalid;
  usize start = 0;
  usize body_begin = 0;
  usize body_end = 0;
  usize terminator = 0;
};

static auto block_addresses(std::span<const Instr> nodes) -> std::vector<BlockAddress> {
  auto blocks = std::vector<BlockAddress>{};
  auto open = option<std::pair<LabelID, usize>>{};
  for (usize index = 0; index < nodes.size(); index++) {
    const auto& ir = nodes[index].ir;
    if (const auto* label = std::get_if<ir::Label>(&ir)) {
      open = std::pair{label->label, index};
    } else if (is_terminator(ir) && open.has_value()) {
      blocks.push_back({
        .label = open->first,
        .start = open->second,
        .body_begin = open->second + 1,
        .body_end = index,
        .terminator = index,
      });
      open = nullopt;
    }
  }

  return blocks;
}

static auto used_values(std::span<const Instr> nodes) -> ankerl::unordered_dense::set<ValueID> {
  auto used = ankerl::unordered_dense::set<ValueID>{};
  for (const auto& node : nodes) {
    visit_operands(node.ir, [&](ValueID id) { used.insert(id); });
  }

  return used;
}

static auto pinned_labels(std::span<const Instr> nodes) -> ankerl::unordered_dense::set<LabelID> {
  auto pinned = ankerl::unordered_dense::set<LabelID>{};
  for (const auto& node : nodes) {
    if (const auto* merge = std::get_if<ir::SelectionMerge>(&node.ir)) {
      pinned.insert(merge->merge);
    }
  }

  return pinned;
}

static auto is_empty_block(
  std::span<const Instr> nodes, const BlockAddress& block, const ankerl::unordered_dense::set<ValueID>& used
) -> bool {
  for (usize index = block.body_begin; index < block.body_end; index++) {
    const auto& node = nodes[index];
    if (intersects(side_effects(node.ir), SideEffect::Observable) || used.contains(node.id)) {
      return false;
    }
  }

  return true;
}

static auto predecessors_of(std::span<const BlockAddress> blocks, std::span<const Instr> nodes, LabelID label)
  -> std::vector<LabelID> {
  auto predecessors = std::vector<LabelID>{};
  for (const auto& block : blocks) {
    const auto& terminator = nodes[block.terminator].ir;
    if (const auto* branch = std::get_if<ir::Branch>(&terminator); branch && branch->target == label) {
      predecessors.push_back(block.label);
    } else if (
      const auto* conditional = std::get_if<ir::BranchConditional>(&terminator);
      conditional && (conditional->true_label == label || conditional->false_label == label)
    ) {
      predecessors.push_back(block.label);
    }
  }

  return predecessors;
}

using RewrittenPhi = std::pair<usize, std::vector<std::pair<ValueID, LabelID>>>;

static auto wire_phis(std::span<const Instr> nodes, LabelID from, LabelID to) -> option<std::vector<RewrittenPhi>> {
  auto rewritten = std::vector<RewrittenPhi>{};
  for (usize index = 0; index < nodes.size(); index++) {
    const auto* phi = std::get_if<ir::Phi>(&nodes[index].ir);
    if (!phi || std::ranges::none_of(phi->incoming, [&](const auto& entry) { return entry.second == from; })) {
      continue;
    }

    auto merged = std::vector<std::pair<ValueID, LabelID>>{};
    for (const auto& [value, label] : phi->incoming) {
      const auto target = label == from ? to : label;
      const auto it = std::ranges::find(merged, target, &std::pair<ValueID, LabelID>::second);
      if (it == merged.end()) {
        merged.emplace_back(value, target);
      } else if (it->first != value) {
        return nullopt;
      }
    }

    rewritten.emplace_back(index, std::move(merged));
  }

  return rewritten;
}

static auto wire_empty_block(std::vector<Instr>& nodes) -> bool {
  const auto blocks = block_addresses(nodes);
  const auto used = used_values(nodes);
  const auto pinned = pinned_labels(nodes);

  for (usize block_index = 1; block_index < blocks.size(); block_index++) {
    const auto& block = blocks[block_index];
    const auto* branch = std::get_if<ir::Branch>(&nodes[block.terminator].ir);
    if (
      !branch || branch->target == block.label || pinned.contains(block.label) || !is_empty_block(nodes, block, used)
    ) {
      continue;
    }

    // more than one way in means the successor's phis would grow an incoming per predecessor
    const auto predecessors = predecessors_of(blocks, nodes, block.label);
    if (predecessors.size() != 1) {
      continue;
    }

    const auto predecessor = predecessors.front();
    auto phis = wire_phis(nodes, block.label, predecessor);
    if (!phis.has_value()) {
      continue;
    }

    const auto from = std::ranges::find(blocks, predecessor, &BlockAddress::label);
    if (from == blocks.end()) {
      continue;
    }

    const auto target = branch->target;
    auto& terminator = nodes[from->terminator].ir;
    if (auto* edge = std::get_if<ir::Branch>(&terminator)) {
      edge->target = target;
    } else if (auto* conditional = std::get_if<ir::BranchConditional>(&terminator)) {
      if (conditional->true_label == block.label) {
        conditional->true_label = target;
      }

      if (conditional->false_label == block.label) {
        conditional->false_label = target;
      }
    } else {
      continue;
    }

    for (auto& [index, incoming] : *phis) {
      nodes[index].ir = ir::Phi{.incoming = std::move(incoming)};
    }

    nodes.erase(
      nodes.begin() + static_cast<std::ptrdiff_t>(block.start),
      nodes.begin() + static_cast<std::ptrdiff_t>(block.terminator + 1)
    );
    return true;
  }

  return false;
}

static auto fold_equal_targets(std::vector<Instr>& nodes) -> bool {
  for (usize index = 0; index < nodes.size(); index++) {
    const auto* conditional = std::get_if<ir::BranchConditional>(&nodes[index].ir);
    if (!conditional || conditional->true_label != conditional->false_label) {
      continue;
    }

    nodes[index].ir = ir::Branch{.target = conditional->true_label};
    if (index > 0 && std::holds_alternative<ir::SelectionMerge>(nodes[index - 1].ir)) {
      nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(index - 1));
    }

    return true;
  }

  return false;
}

auto simplify_cfg(std::vector<Instr> nodes) -> std::vector<Instr> {
  while (true) {
    auto is_changed = wire_empty_block(nodes);
    is_changed |= fold_equal_targets(nodes);
    if (!is_changed) {
      return nodes;
    }
  }
}

static auto access_constants(std::span<const Instr> nodes) -> ankerl::unordered_dense::map<ValueID, Access> {
  auto accesses = ankerl::unordered_dense::map<ValueID, Access>{};
  for (const auto& node : nodes) {
    if (
      const auto* constant = std::get_if<ir::Constant>(&node.ir);
      constant && constant->kind == ir::Constant::Kind::Access
    ) {
      accesses.emplace(node.id, constant->as_access());
    }
  }

  return accesses;
}

static auto barrier_runs(std::span<const Instr> nodes) -> std::vector<std::vector<usize>> {
  auto runs = std::vector<std::vector<usize>>{};
  auto run = std::vector<usize>{};
  const auto close = [&] {
    if (run.size() > 1) {
      runs.push_back(std::move(run));
    }

    run.clear();
  };

  for (usize index = 0; index < nodes.size(); index++) {
    const auto& ir = nodes[index].ir;
    if (std::holds_alternative<ir::MemoryBarrier>(ir)) {
      run.push_back(index);
    } else if (std::holds_alternative<ir::ImageBarrier>(ir) || !intersects(side_effects(ir), SideEffect::Observable)) {
      continue;
    } else {
      close();
    }
  }

  close();
  return runs;
}

struct Folded {
  usize at = 0;
  Access src = Access::Empty;
  Access dst = Access::Empty;
};

auto fold_barriers(std::vector<Instr> nodes, u32& next_id) -> std::vector<Instr> {
  const auto accesses = access_constants(nodes);
  auto kept = std::vector<Folded>{};
  auto dropped = ankerl::unordered_dense::set<usize>{};
  for (const auto& run : barrier_runs(nodes)) {
    const auto first = kept.size();
    for (const auto at : run) {
      const auto& barrier = std::get<ir::MemoryBarrier>(nodes[at].ir);
      const auto src = accesses.find(barrier.src_access);
      const auto dst = accesses.find(barrier.dst_access);
      if (src == accesses.end() || dst == accesses.end()) {
        continue;
      }

      if (kept.size() > first) {
        kept[first].src |= src->second;
        kept[first].dst |= dst->second;
        dropped.insert(at);
      } else {
        kept.push_back({.at = at, .src = src->second, .dst = dst->second});
      }
    }
  }

  if (dropped.empty()) {
    return nodes;
  }

  auto pool = ankerl::unordered_dense::map<Access, ValueID>{};
  for (const auto& [id, access] : accesses) {
    pool.emplace(access, id);
  }

  auto result = std::vector<Instr>{};
  result.reserve(nodes.size());
  const auto constant_for = [&](Access access) -> ValueID {
    if (const auto it = pool.find(access); it != pool.end()) {
      return it->second;
    }

    const auto id = static_cast<ValueID>(next_id++);
    pool.emplace(access, id);
    result.push_back({.id = id, .ir = ir::Constant::from_access(access)});
    return id;
  };

  for (usize index = 0; index < nodes.size(); index++) {
    if (dropped.contains(index)) {
      continue;
    }

    const auto folded = std::ranges::find(kept, index, &Folded::at);
    if (folded == kept.end()) {
      result.push_back(std::move(nodes[index]));
      continue;
    }

    const auto src_access = constant_for(folded->src);
    const auto dst_access = constant_for(folded->dst);
    result.push_back(
      {.id = nodes[index].id, .ir = ir::MemoryBarrier{.src_access = src_access, .dst_access = dst_access}}
    );
  }

  return result;
}

auto normalize_barriers(std::vector<Instr> nodes) -> std::vector<Instr> {
  ZoneScoped;
  const auto accesses = access_constants(nodes);
  const auto same_access = [&](ValueID first, ValueID second) -> bool {
    return first == second ||
           (accesses.contains(first) && accesses.contains(second) && accesses.at(first) == accesses.at(second));
  };
  auto result = std::vector<Instr>{};
  result.reserve(nodes.size());
  for (usize start = 0; start < nodes.size();) {
    if (!is_barrier(nodes[start].ir)) {
      result.push_back(std::move(nodes[start++]));
      continue;
    }

    auto end = start;
    while (end < nodes.size() && is_barrier(nodes[end].ir)) {
      end++;
    }
    // put memory dependencies first so independent image barriers stay consecutive
    for (auto index = start; index < end; index++) {
      if (std::holds_alternative<ir::MemoryBarrier>(nodes[index].ir)) {
        result.push_back(std::move(nodes[index]));
      }
    }

    auto independent = result.size();
    for (auto index = start; index < end; index++) {
      const auto* image = std::get_if<ir::ImageBarrier>(&nodes[index].ir);
      if (!image) {
        continue;
      }
      const auto same_transition = [&](const Instr& held) -> bool {
        const auto& other = std::get<ir::ImageBarrier>(held.ir);
        return other.value == image->value && other.old_layout == image->old_layout &&
               other.new_layout == image->new_layout && same_access(other.src_access, image->src_access) &&
               same_access(other.dst_access, image->dst_access);
      };
      auto held_images = std::span(result).subspan(independent);
      if (std::ranges::any_of(held_images, [&](const Instr& held) -> bool {
            return same_transition(held) && std::get<ir::ImageBarrier>(held.ir).range.contains(image->range);
          })) {
        continue;
      }
      if (std::ranges::any_of(held_images, [&](const Instr& held) -> bool {
            const auto& other = std::get<ir::ImageBarrier>(held.ir);
            return other.value == image->value && other.range.overlaps(image->range);
          })) {
        independent = result.size();
      }
      result.push_back(std::move(nodes[index]));
      // neighbouring ranges may become mergeable after another image joins the run
      for (auto first = independent; first < result.size(); first++) {
        for (auto second = first + 1; second < result.size();) {
          auto& held = std::get<ir::ImageBarrier>(result[first].ir);
          const auto& other = std::get<ir::ImageBarrier>(result[second].ir);
          if (
            held.value == other.value && held.old_layout == other.old_layout && held.new_layout == other.new_layout &&
            same_access(held.src_access, other.src_access) && same_access(held.dst_access, other.dst_access) &&
            merge_barrier_ranges(held.range, other.range)
          ) {
            result.erase(result.begin() + static_cast<std::ptrdiff_t>(second));
            second = first + 1;
          } else {
            second++;
          }
        }
      }
    }
    start = end;
  }
  return result;
}

struct BarrierScope {
  bool is_memory = false;
  ValueID root = ValueID::Invalid;
};

static auto is_fixed(const IR& ir) -> bool {
  // moving a barrier above a pass would make the pass wait on the barrier's source
  const auto fixed = SideEffect::Control | SideEffect::External | SideEffect::Host;
  return intersects(side_effects(ir), fixed) || opens_region(ir) || closes_region(ir);
}

static auto barrier_scope(const Module& module, const IR& ir) -> option<BarrierScope> {
  if (const auto* image = std::get_if<ir::ImageBarrier>(&ir)) {
    return BarrierScope{.root = module.resource_root(image->value)};
  }

  if (std::holds_alternative<ir::MemoryBarrier>(ir)) {
    return BarrierScope{.is_memory = true};
  }

  return nullopt;
}

static auto touches(const Module& module, const BarrierScope& scope, const IR& ir, std::vector<ValueID>& uses) -> bool {
  if (const auto other = barrier_scope(module, ir); other.has_value()) {
    return !scope.is_memory && !other->is_memory && scope.root == other->root;
  }

  uses.clear();
  module.resource_uses(ir, uses);
  if (scope.is_memory) {
    // a memory barrier doesn't say which buffer it is for
    return std::ranges::any_of(uses, [&](ValueID resource) { return module.is_buffer(resource); });
  }

  return std::ranges::find(uses, scope.root) != uses.end();
}

auto Module::schedule_barriers(this const Module& self, std::vector<Instr> nodes) -> std::vector<Instr> {
  auto result = std::vector<Instr>{};
  result.reserve(nodes.size());
  auto operands = std::vector<ValueID>{};
  auto uses = std::vector<ValueID>{};

  for (auto& node : nodes) {
    const auto scope = barrier_scope(self, node.ir);
    if (!scope.has_value()) {
      // a command no pending barrier is for goes ahead of them, so the barriers it lets through can
      // join the ones the next command needs
      auto run = 0_sz;
      while (run < result.size() && is_barrier(result[result.size() - 1 - run].ir)) {
        run += 1;
      }

      const auto start = result.size() - run;
      auto is_free = run > 0 && !is_fixed(node.ir);
      for (auto index = start; is_free && index < result.size(); index++) {
        const auto barrier = barrier_scope(self, result[index].ir);
        is_free = barrier.has_value() && !touches(self, *barrier, node.ir, uses);
      }

      if (is_free) {
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(start), std::move(node));
      } else {
        result.push_back(std::move(node));
      }

      continue;
    }

    operands.clear();
    visit_operands(node.ir, [&](ValueID operand) { operands.push_back(operand); });

    // a barrier moves only to join an earlier run, moving alone batches nothing
    auto after = 0_sz;
    for (auto index = result.size(); index > 0; index--) {
      const auto& held = result[index - 1];
      const auto holds_back = std::ranges::find(operands, held.id) != operands.end() || is_fixed(held.ir) ||
                              touches(self, *scope, held.ir, uses);
      if (holds_back) {
        after = index;
        break;
      }
    }

    auto at = result.size();
    for (auto index = after; index < result.size(); index++) {
      if (is_barrier(result[index].ir)) {
        at = index;
        while (at < result.size() && is_barrier(result[at].ir)) {
          at += 1;
        }

        break;
      }
    }

    result.insert(result.begin() + static_cast<std::ptrdiff_t>(at), std::move(node));
  }

  return result;
}

// the attachment the executor sizes a rendering pass from when the pass names no area
static auto sizing_attachment(const Module& module, std::span<const std::pair<ValueID, ValueID>> attachments)
  -> option<ValueID> {
  for (const auto range : {Access::ColorRW, Access::DepthStencilRW}) {
    for (const auto& [resource, access] : attachments) {
      if (intersects(module.resolve_access(access), range)) {
        const auto elements = module.resource_elements(resource);
        if (!elements.empty()) {
          return elements.front();
        }
      }
    }
  }

  return nullopt;
}

// a load op clears only the render area, so the area has to be the whole image
static auto pass_covers(const Module& module, const ir::BeginRendering& pass, ValueID cleared, ValueID source) -> bool {
  if (pass.layer_count != 1) {
    return false;
  }

  const auto sizing = sizing_attachment(module, pass.attachments);
  if (!is_valid(pass.render_area) && sizing == cleared) {
    return true;
  }

  const auto extent_of = [&](ValueID image) -> option<Extent2D> {
    const auto extent = module.image_extent(image);
    if (!extent.has_value()) {
      return nullopt;
    }

    return Extent2D{extent->width, extent->height};
  };
  auto area = option<Extent2D>{};
  if (is_valid(pass.render_area)) {
    area = module.extent_2d(pass.render_area);
  } else if (sizing.has_value()) {
    area = extent_of(*sizing);
  }

  const auto image = extent_of(source);
  return area.has_value() && image.has_value() && area->width == image->width && area->height == image->height;
}

// a load op clears the one mip and layer the view renders to, and only the depth aspect
static auto is_single_image(const Module& module, ValueID source) -> bool {
  const auto elements = module.resource_elements(source);
  if (elements.size() != 1 || elements.front() != source) {
    return false;
  }

  const auto* resource = module.resolve_resource(source);
  if (!resource) {
    return false;
  }

  if (std::holds_alternative<ir::SwapchainImage>(*resource)) {
    return true;
  }

  const auto* construct = std::get_if<ir::ConstructImage>(resource);
  if (!construct) {
    return false;
  }

  const auto is_one = [&](ValueID id) {
    const auto* constant = std::get_if<ir::Constant>(&module.get(id));
    return constant && constant->kind == ir::Constant::Kind::U32 && constant->as_u32() == 1;
  };
  return is_one(construct->level_count) && is_one(construct->layer_count) &&
         (aspect_mask(construct->format) & VK_IMAGE_ASPECT_STENCIL_BIT) == 0 &&
         module.resource_range(source).is_whole();
}

// a clear whose result only opens rendering passes as a written attachment becomes those
// attachments' load op, which leaves no transfer to synchronize
auto Module::fold_clears(this const Module& self, std::vector<Instr> nodes) -> std::vector<Instr> {
  auto uses = ankerl::unordered_dense::map<ValueID, std::vector<usize>>{};
  for (usize index = 0; index < nodes.size(); index++) {
    visit_operands(nodes[index].ir, [&](ValueID operand) { uses[operand].push_back(index); });
  }

  auto folded = ankerl::unordered_dense::map<ValueID, std::pair<ValueID, ValueID>>{};
  for (const auto& node : nodes) {
    const auto* clear = std::get_if<ir::Clear>(&node.ir);
    if (!clear) {
      continue;
    }

    const auto users = uses.find(node.id);
    if (users == uses.end() || !is_single_image(self, clear->attachment)) {
      continue;
    }

    auto is_rendered = false;
    auto is_foldable = true;
    for (const auto index : users->second) {
      const auto& user = nodes[index].ir;
      if (const auto* pass = std::get_if<ir::BeginRendering>(&user)) {
        is_rendered = true;
        const auto is_written = std::ranges::all_of(pass->attachments, [&](const auto& attachment) {
          const auto access = self.resolve_access(attachment.second);
          return attachment.first != node.id ||
                 (writes(access) && intersects(access, Access::ColorRW | Access::DepthStencilRW));
        });
        is_foldable &= is_written && pass_covers(self, *pass, node.id, clear->attachment);
      } else if (const auto* result = std::get_if<ir::PassResult>(&user)) {
        is_foldable &= result->resource == node.id;
      } else {
        is_foldable = false;
      }
    }

    if (is_foldable && is_rendered) {
      folded.emplace(node.id, std::pair{clear->attachment, clear->color});
    }
  }

  if (folded.empty()) {
    return nodes;
  }

  std::erase_if(nodes, [&](const Instr& node) { return folded.contains(node.id); });
  for (auto& node : nodes) {
    if (auto* pass = std::get_if<ir::BeginRendering>(&node.ir)) {
      for (auto& [resource, access] : pass->attachments) {
        if (const auto it = folded.find(resource); it != folded.end()) {
          resource = it->second.first;
          pass->clears.emplace_back(it->second.first, it->second.second);
        }
      }
    } else if (auto* result = std::get_if<ir::PassResult>(&node.ir)) {
      if (const auto it = folded.find(result->resource); it != folded.end()) {
        result->resource = it->second.first;
      }
    }
  }

  return nodes;
}
} // namespace ox::vir
