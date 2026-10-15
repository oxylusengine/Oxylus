#include <cstddef>

#include "Render/Graph/Module.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
struct ImageState {
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  ValueID last_access = ValueID::Invalid;
  Access access = Access::Empty;

  auto operator==(const ImageState&) const -> bool = default;
};

// the states the parts of one image are in, as rectangles of mips and layers that never overlap
struct ImageSegment {
  Subresource range = {};
  ImageState state = {};
};

using ImageStates = ankerl::unordered_dense::map<ValueID, std::vector<ImageSegment>>;

struct BufferState {
  ValueID last_access = ValueID::Invalid;
  Access access = Access::Empty;
};

using BufferStates = ankerl::unordered_dense::map<ValueID, BufferState>;

static auto make_range(u64 base_level, u64 level_end, u64 base_layer, u64 layer_end) -> Subresource {
  return {
    .base_level = static_cast<u32>(base_level),
    .level_count = level_end == REMAINING ? REMAINING : static_cast<u32>(level_end - base_level),
    .base_layer = static_cast<u32>(base_layer),
    .layer_count = layer_end == REMAINING ? REMAINING : static_cast<u32>(layer_end - base_layer),
  };
}

// cuts `segment` along `cut` so every piece lies either wholly inside it or wholly outside
static auto split(const ImageSegment& segment, const Subresource& cut, std::vector<ImageSegment>& out) -> void {
  const auto& range = segment.range;
  if (!range.overlaps(cut) || cut.contains(range)) {
    out.push_back(segment);
    return;
  }

  const auto inner = range.intersect(cut);
  const u64 level_end = range.level_end();
  const u64 inner_level_end = inner.level_end();
  if (inner.base_level > range.base_level) {
    out.push_back({make_range(range.base_level, inner.base_level, range.base_layer, range.layer_end()), segment.state});
  }

  if (inner_level_end < level_end) {
    out.push_back({make_range(inner_level_end, level_end, range.base_layer, range.layer_end()), segment.state});
  }

  if (inner.base_layer > range.base_layer) {
    out.push_back({make_range(inner.base_level, inner_level_end, range.base_layer, inner.base_layer), segment.state});
  }

  if (inner.layer_end() < range.layer_end()) {
    out.push_back({make_range(inner.base_level, inner_level_end, inner.layer_end(), range.layer_end()), segment.state});
  }

  out.push_back({inner, segment.state});
}

// merges neighbours left in the same state, so whole-image accesses go back to one barrier
static auto coalesce(std::vector<ImageSegment>& segments) -> void {
  auto is_merged = true;
  while (is_merged) {
    is_merged = false;
    for (usize i = 0; i < segments.size() && !is_merged; i++) {
      for (usize j = i + 1; j < segments.size() && !is_merged; j++) {
        auto& a = segments[i].range;
        const auto& b = segments[j].range;
        if (segments[i].state != segments[j].state) {
          continue;
        }

        const auto same_layers = a.base_layer == b.base_layer && a.layer_count == b.layer_count;
        const auto same_levels = a.base_level == b.base_level && a.level_count == b.level_count;
        if (same_layers && a.level_end() == b.base_level) {
          a = make_range(a.base_level, b.level_end(), a.base_layer, a.layer_end());
        } else if (same_layers && b.level_end() == a.base_level) {
          a = make_range(b.base_level, a.level_end(), a.base_layer, a.layer_end());
        } else if (same_levels && a.layer_end() == b.base_layer) {
          a = make_range(a.base_level, a.level_end(), a.base_layer, b.layer_end());
        } else if (same_levels && b.layer_end() == a.base_layer) {
          a = make_range(a.base_level, a.level_end(), b.base_layer, a.layer_end());
        } else {
          continue;
        }

        segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(j));
        is_merged = true;
      }
    }
  }
}

struct Construct {
  LabelID merge = LabelID::Invalid;
  ImageStates entry_images = {};
  BufferStates entry_buffers = {};
  struct Arm {
    usize at = 0;
    ImageStates images = {};
    BufferStates buffers = {};
  };
  std::vector<Arm> arms = {};
};

struct SyncState {
  const Module* module = nullptr;
  std::vector<Instr>* result = nullptr;
  u32* next_id = nullptr;
  ImageStates images = {};
  BufferStates buffers = {};
  ValueID no_access = ValueID::Invalid;
  ValueID host_write = ValueID::Invalid;
  ankerl::unordered_dense::map<Access, ValueID> read_accesses = {};
  std::vector<std::pair<Access, Access>> emitted_memory = {};

  auto alloc(this SyncState& self) -> ValueID {
    OX_ASSERT(*self.next_id != ~0_u32, "exhausted valid value ids");
    return static_cast<ValueID>((*self.next_id)++);
  }

  auto emit_access(this SyncState& self, Access access) -> ValueID {
    const auto id = self.alloc();
    self.result->push_back({.id = id, .ir = ir::Constant::from_access(access)});
    return id;
  }

  // a read is shared between every barrier that waits on it, a write always gets a constant of
  // its own so it stays a barrier point
  auto read_access(this SyncState& self, Access access) -> ValueID {
    if (const auto it = self.read_accesses.find(access); it != self.read_accesses.end()) {
      return it->second;
    }

    const auto id = self.emit_access(access);
    self.read_accesses.emplace(access, id);
    return id;
  }

  auto access_id(this SyncState& self, Access access) -> ValueID {
    return writes(access) ? self.emit_access(access) : self.read_access(access);
  }

  auto host_write_id(this SyncState& self) -> ValueID {
    if (!is_valid(self.host_write)) {
      self.host_write = self.emit_access(Access::HostWrite);
    }

    return self.host_write;
  }

  // what a resource rests at until the module touches it, a resource the graph allocates for this
  // run rests at nothing since there is no earlier use to wait for
  auto resting_access(this SyncState& self, Access access) -> ValueID {
    if (access == Access::None) {
      return self.no_access;
    }

    if (access == Access::HostWrite) {
      return self.host_write_id();
    }

    return self.access_id(access);
  }

  auto undefined(this const SyncState& self) -> ImageState {
    return {.layout = VK_IMAGE_LAYOUT_UNDEFINED, .last_access = self.no_access, .access = Access::None};
  }

  auto emit_image_barrier(
    this SyncState& self,
    const ImageState& from,
    ValueID dst_access,
    VkImageLayout new_layout,
    ValueID root,
    const Subresource& range
  ) -> void {
    self.result->push_back({
      .id = self.alloc(),
      .ir = ir::ImageBarrier{
        .src_access = from.last_access,
        .dst_access = dst_access,
        .old_layout = from.layout,
        .new_layout = new_layout,
        .value = root,
        .range = range,
      },
    });
  }

  auto emit_memory_barrier(this SyncState& self, const BufferState& from, const BufferState& to) -> void {
    const auto pair = std::pair{from.access, to.access};
    if (std::ranges::find(self.emitted_memory, pair) != self.emitted_memory.end()) {
      return;
    }

    self.emitted_memory.push_back(pair);
    self.result->push_back({
      .id = self.alloc(),
      .ir = ir::MemoryBarrier{.src_access = from.last_access, .dst_access = to.last_access},
    });
  }

  // a buffer the host filled is visible to the GPU only after a barrier, and stays visible until
  // something writes it again
  auto buffer_barrier(this SyncState& self, ValueID root, Access access) -> void {
    auto last = BufferState{.last_access = self.host_write_id(), .access = Access::HostWrite};
    if (const auto it = self.buffers.find(root); it != self.buffers.end()) {
      last = it->second;
    }

    const auto next = BufferState{.last_access = self.access_id(access), .access = access};
    if (last.last_access != next.last_access) {
      if (last.access != Access::None) {
        self.emit_memory_barrier(last, next);
      }

      self.buffers.insert_or_assign(root, next);
    }
  }

  auto segments_of(this SyncState& self, ValueID root) -> std::vector<ImageSegment>& {
    auto it = self.images.find(root);
    if (it == self.images.end()) {
      it = self.images.emplace(root, std::vector{ImageSegment{.state = self.undefined()}}).first;
    }

    return it->second;
  }

  auto transition(this SyncState& self, ValueID resource, ValueID access_id, Access access, Subresource relative = {})
    -> void {
    const auto root = self.module->resource_root(resource);
    const auto viewed = self.module->resource_range(resource);
    const auto range = viewed.intersect(
      Subresource{
        .base_level = viewed.base_level + relative.base_level,
        .level_count = relative.level_count,
        .base_layer = viewed.base_layer + relative.base_layer,
        .layer_count = relative.layer_count
      }
    );
    auto& segments = self.segments_of(root);

    auto pieces = std::vector<ImageSegment>{};
    for (const auto& segment : segments) {
      split(segment, range, pieces);
    }

    const auto new_layout = to_image_layout(access);
    for (auto& piece : pieces) {
      if (!range.contains(piece.range)) {
        continue;
      }

      auto& state = piece.state;
      if (state.layout == new_layout && !writes(state.access) && !writes(access)) {
        const auto merged = state.access | access;
        if (merged == state.access) {
          continue;
        }

        if (state.access != Access::None) {
          self.emit_image_barrier(state, access_id, new_layout, root, piece.range);
        }

        state = {.layout = new_layout, .last_access = self.read_access(merged), .access = merged};
        continue;
      }

      self.emit_image_barrier(state, access_id, new_layout, root, piece.range);
      state = {.layout = new_layout, .last_access = access_id, .access = access};
    }

    coalesce(pieces);
    segments = std::move(pieces);
  }

  // the load op overwrites the contents, but the last writer is still waited on
  auto discard(this SyncState& self, ValueID resource) -> void {
    const auto root = self.module->resource_root(resource);
    const auto range = self.module->resource_range(resource);
    auto& segments = self.segments_of(root);
    auto pieces = std::vector<ImageSegment>{};
    for (const auto& segment : segments) {
      split(segment, range, pieces);
    }

    for (auto& piece : pieces) {
      if (range.contains(piece.range)) {
        piece.state.layout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
    }

    coalesce(pieces);
    segments = std::move(pieces);
  }

  auto layout_of(this SyncState& self, ValueID resource) -> VkImageLayout {
    const auto root = self.module->resource_root(resource);
    const auto range = self.module->resource_range(resource);
    for (const auto& segment : self.segments_of(root)) {
      if (segment.range.overlaps(range)) {
        return segment.state.layout;
      }
    }

    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
};

// a selection leaves a resource in one of two states, so every arm but the first is brought into
// agreement with the first before control joins
static auto join_arms(
  SyncState& sync, Construct construct, usize predecessors, const ankerl::unordered_dense::set<ValueID>& live
) -> std::pair<ImageStates, BufferStates> {
  const auto is_join_from_arm = !construct.arms.empty() && construct.arms.size() == predecessors;
  auto join_images = is_join_from_arm ? construct.arms.front().images : construct.entry_images;
  auto join_buffers = is_join_from_arm ? construct.arms.front().buffers : construct.entry_buffers;

  auto images = std::vector<std::pair<ValueID, const std::vector<ImageSegment>*>>{};
  for (const auto& [root, segments] : join_images) {
    if (live.contains(root)) {
      images.emplace_back(root, &segments);
    }
  }

  std::ranges::sort(images, {}, [](const auto& entry) { return std::to_underlying(entry.first); });
  auto buffers = std::vector<std::pair<ValueID, BufferState>>{};
  for (const auto& [root, state] : join_buffers) {
    if (live.contains(root)) {
      buffers.emplace_back(root, state);
    }
  }

  std::ranges::sort(buffers, {}, [](const auto& entry) { return std::to_underlying(entry.first); });

  // an insertion shifts everything after it, so arms are patched back to front
  const auto first_to_patch = is_join_from_arm ? 1_sz : 0_sz;
  for (auto arm_index = construct.arms.size(); arm_index > first_to_patch; arm_index--) {
    auto& arm = construct.arms[arm_index - 1];
    auto barriers = std::vector<Instr>{};
    auto patch = SyncState{.module = sync.module, .result = &barriers, .next_id = sync.next_id};

    for (const auto& [root, targets] : images) {
      auto current = std::vector<ImageSegment>{ImageSegment{.state = sync.undefined()}};
      if (const auto it = arm.images.find(root); it != arm.images.end()) {
        current = it->second;
      } else if (const auto entry = construct.entry_images.find(root); entry != construct.entry_images.end()) {
        current = entry->second;
      }

      for (const auto& target : *targets) {
        for (const auto& segment : current) {
          if (!segment.range.overlaps(target.range)) {
            continue;
          }

          if (segment.state.layout == target.state.layout && segment.state.access == target.state.access) {
            continue;
          }

          patch.emit_image_barrier(
            segment.state,
            target.state.last_access,
            target.state.layout,
            root,
            segment.range.intersect(target.range)
          );
        }
      }
    }

    auto emitted = std::vector<std::pair<Access, Access>>{};
    for (const auto& [root, target] : buffers) {
      auto current = BufferState{.last_access = sync.no_access, .access = Access::None};
      if (const auto it = arm.buffers.find(root); it != arm.buffers.end()) {
        current = it->second;
      }

      if (current.access == target.access || current.access == Access::None) {
        continue;
      }

      if (std::ranges::find(emitted, std::pair{current.access, target.access}) != emitted.end()) {
        continue;
      }

      emitted.emplace_back(current.access, target.access);
      barriers.push_back({
        .id = patch.alloc(),
        .ir = ir::MemoryBarrier{.src_access = current.last_access, .dst_access = target.last_access},
      });
    }

    sync.result->insert(
      sync.result->begin() + static_cast<std::ptrdiff_t>(arm.at),
      std::make_move_iterator(barriers.begin()),
      std::make_move_iterator(barriers.end())
    );
  }

  return {std::move(join_images), std::move(join_buffers)};
}

auto Module::sync(this const Module& self, std::vector<Instr> nodes, u32& next_id) -> std::vector<Instr> {
  ZoneScoped;

  auto access_values = ankerl::unordered_dense::map<ValueID, Access>{};
  for (const auto& node : nodes) {
    if (
      const auto* constant = std::get_if<ir::Constant>(&node.ir);
      constant && constant->kind == ir::Constant::Kind::Access
    ) {
      access_values.emplace(node.id, constant->as_access());
    }
  }

  const auto resolve = [&](ValueID id) -> Access {
    const auto it = access_values.find(id);
    OX_ASSERT(it != access_values.end(), "%%%u is not an access constant", std::to_underlying(id));
    return it->second;
  };

  // how many ways there are into each block, deciding whether an arm's end state can stand as the
  // join or whether control can also fall straight through
  auto predecessors = ankerl::unordered_dense::map<LabelID, usize>{};
  for (const auto& node : nodes) {
    if (const auto* branch = std::get_if<ir::Branch>(&node.ir)) {
      predecessors[branch->target] += 1;
    } else if (const auto* conditional = std::get_if<ir::BranchConditional>(&node.ir)) {
      predecessors[conditional->true_label] += 1;
      predecessors[conditional->false_label] += 1;
    }
  }

  // what each pass touches besides its declared resources: descriptor writes and vertex input,
  // all of which are synchronized once when the pass opens
  struct RegionUse {
    ValueID resource = ValueID::Invalid;
    Access access = Access::Empty;
  };
  auto region_images = ankerl::unordered_dense::map<ValueID, std::vector<RegionUse>>{};
  auto region_buffers = ankerl::unordered_dense::map<ValueID, std::vector<RegionUse>>{};
  auto open_region = option<ValueID>{};
  const auto merge_use = [&](std::vector<RegionUse>& uses, ValueID resource, Access access, bool is_image) {
    const auto root = self.resource_root(resource);
    const auto range = self.resource_range(resource);
    auto it = std::ranges::find_if(uses, [&](const RegionUse& use) {
      return self.resource_root(use.resource) == root && (!is_image || self.resource_range(use.resource) == range);
    });
    if (it != uses.end()) {
      it->access |= access;
    } else {
      uses.push_back({.resource = is_image ? resource : root, .access = access});
    }
  };

  for (const auto& node : nodes) {
    if (opens_region(node.ir)) {
      open_region = node.id;
      continue;
    }

    if (closes_region(node.ir)) {
      open_region = nullopt;
      continue;
    }

    if (!open_region.has_value()) {
      continue;
    }

    if (const auto* write = std::get_if<ir::WriteDescriptor>(&node.ir)) {
      const auto access = resolve(write->access);
      if (access == Access::None) {
        continue;
      }

      if (const auto image = write->descriptor.image_value(); image.has_value()) {
        merge_use(region_images[*open_region], *image, access, true);
      } else if (const auto buffer = write->descriptor.buffer_value(); buffer.has_value()) {
        merge_use(region_buffers[*open_region], *buffer, access, false);
      }
    } else if (const auto* vertex = std::get_if<ir::BindVertexBuffers>(&node.ir)) {
      for (const auto buffer : self.array_elements(vertex->buffers)) {
        merge_use(region_buffers[*open_region], buffer, Access::AttributeRead, false);
      }
    } else if (const auto* index = std::get_if<ir::BindIndexBuffer>(&node.ir)) {
      merge_use(region_buffers[*open_region], index->buffer, Access::IndexRead, false);
    }
  }

  // which resources are still named past each label, walked backwards so every label sees what
  // comes after it
  auto live_after = ankerl::unordered_dense::map<LabelID, ankerl::unordered_dense::set<ValueID>>{};
  {
    auto live = ankerl::unordered_dense::set<ValueID>{};
    auto uses = std::vector<ValueID>{};
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      if (const auto* label = std::get_if<ir::Label>(&it->ir)) {
        live_after.insert_or_assign(label->label, live);
      }

      uses.clear();
      self.resource_uses(it->ir, uses);
      live.insert(uses.begin(), uses.end());
    }
  }

  auto result = std::vector<Instr>{};
  result.reserve(nodes.size() * 2);
  auto sync = SyncState{.module = &self, .result = &result, .next_id = &next_id};
  sync.no_access = sync.emit_access(Access::None);

  auto constructs = std::vector<Construct>{};
  for (auto& node : nodes) {
    sync.emitted_memory.clear();
    const auto id = node.id;
    auto& ir_node = node.ir;

    if (const auto* merge = std::get_if<ir::SelectionMerge>(&ir_node)) {
      constructs.push_back({.merge = merge->merge, .entry_images = sync.images, .entry_buffers = sync.buffers});
    } else if (
      const auto* branch = std::get_if<ir::Branch>(&ir_node);
      branch && !constructs.empty() && constructs.back().merge == branch->target
    ) {
      auto& construct = constructs.back();
      construct.arms.push_back({
        .at = result.size(),
        .images = std::exchange(sync.images, construct.entry_images),
        .buffers = std::exchange(sync.buffers, construct.entry_buffers),
      });
    } else if (
      const auto* label = std::get_if<ir::Label>(&ir_node);
      label && !constructs.empty() && constructs.back().merge == label->label
    ) {
      auto construct = std::move(constructs.back());
      constructs.pop_back();
      const auto ways_in = predecessors.contains(label->label) ? predecessors[label->label] : 0;
      const auto live = live_after.contains(label->label) ? live_after[label->label]
                                                          : ankerl::unordered_dense::set<ValueID>{};
      auto [images, buffers] = join_arms(sync, std::move(construct), ways_in, live);
      sync.images = std::move(images);
      sync.buffers = std::move(buffers);
    } else if (const auto* image = std::get_if<ir::ConstructImage>(&ir_node)) {
      const auto access = resolve(image->initial_access);
      sync.images.insert_or_assign(
        id,
        std::vector{ImageSegment{
          .state = {.layout = image->initial_layout, .last_access = sync.resting_access(access), .access = access},
        }}
      );
    } else if (const auto* buffer = std::get_if<ir::ConstructBuffer>(&ir_node)) {
      const auto access = resolve(buffer->initial_access);
      sync.buffers.insert_or_assign(id, BufferState{.last_access = sync.resting_access(access), .access = access});
    } else if (const auto* clear = std::get_if<ir::Clear>(&ir_node)) {
      sync.transition(clear->attachment, sync.emit_access(Access::Clear), Access::Clear);
    } else if (const auto* blit = std::get_if<ir::Blit>(&ir_node)) {
      sync.transition(blit->src, sync.emit_access(Access::BlitRead), Access::BlitRead);
      sync.transition(blit->dst, sync.emit_access(Access::BlitWrite), Access::BlitWrite);
    } else if (const auto* copy = std::get_if<ir::CopyBufferToImage>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(copy->buffer), Access::CopyRead);
      const auto range = copy->region.has_value() ? Subresource{
        .base_level = copy->region->mip_level, .level_count = 1, .base_layer = copy->region->base_layer,
        .layer_count = copy->region->layer_count == 0 ? REMAINING : copy->region->layer_count} : Subresource{};
      sync.transition(copy->image, sync.emit_access(Access::CopyWrite), Access::CopyWrite, range);
    } else if (const auto* readback = std::get_if<ir::CopyImageToBuffer>(&ir_node)) {
      const auto range = readback->region.has_value() ? Subresource{
        .base_level = readback->region->mip_level, .level_count = 1, .base_layer = readback->region->base_layer,
        .layer_count = readback->region->layer_count == 0 ? REMAINING : readback->region->layer_count} : Subresource{};
      sync.transition(readback->image, sync.read_access(Access::CopyRead), Access::CopyRead, range);
      sync.buffer_barrier(self.resource_root(readback->buffer), Access::CopyWrite);
    } else if (const auto* copy_buffer = std::get_if<ir::CopyBuffer>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(copy_buffer->src), Access::CopyRead);
      sync.buffer_barrier(self.resource_root(copy_buffer->dst), Access::CopyWrite);
    } else if (const auto* fill = std::get_if<ir::FillBuffer>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(fill->buffer), Access::Clear);
    } else if (const auto* update = std::get_if<ir::UpdateBuffer>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(update->buffer), Access::Clear);
    } else if (const auto* copy_image = std::get_if<ir::CopyImage>(&ir_node)) {
      const auto layers = copy_image->region.has_value() && copy_image->region->layer_count != 0
                            ? copy_image->region->layer_count
                            : REMAINING;
      const auto src_range =
        copy_image->region.has_value()
          ? Subresource{.base_level = copy_image->region->src_mip, .level_count = 1, .base_layer = copy_image->region->src_layer, .layer_count = layers}
          : Subresource{};
      const auto dst_range =
        copy_image->region.has_value()
          ? Subresource{.base_level = copy_image->region->dst_mip, .level_count = 1, .base_layer = copy_image->region->dst_layer, .layer_count = layers}
          : Subresource{};
      sync.transition(copy_image->src, sync.read_access(Access::CopyRead), Access::CopyRead, src_range);
      sync.transition(copy_image->dst, sync.emit_access(Access::CopyWrite), Access::CopyWrite, dst_range);
    } else if (const auto* resolved = std::get_if<ir::ResolveImage>(&ir_node)) {
      const auto layers = resolved->region.has_value() && resolved->region->layer_count != 0
                            ? resolved->region->layer_count
                            : REMAINING;
      const auto src_range =
        resolved->region.has_value()
          ? Subresource{.base_level = resolved->region->src_mip, .level_count = 1, .base_layer = resolved->region->src_layer, .layer_count = layers}
          : Subresource{};
      const auto dst_range =
        resolved->region.has_value()
          ? Subresource{.base_level = resolved->region->dst_mip, .level_count = 1, .base_layer = resolved->region->dst_layer, .layer_count = layers}
          : Subresource{};
      sync.transition(resolved->src, sync.read_access(Access::ResolveRead), Access::ResolveRead, src_range);
      sync.transition(resolved->dst, sync.emit_access(Access::ResolveWrite), Access::ResolveWrite, dst_range);
    } else if (const auto* builds = std::get_if<ir::BuildAccelerationStructures>(&ir_node)) {
      for (const auto& [resource, access] : builds->resources) {
        sync.buffer_barrier(self.resource_root(resource), resolve(access));
      }
    } else if (const auto* properties = std::get_if<ir::WriteAccelerationStructureProperties>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(properties->source), Access::AccelerationStructureBuildRead);
    } else if (const auto* copied = std::get_if<ir::CopyAccelerationStructure>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(copied->src), Access::AccelerationStructureBuildRead);
      sync.buffer_barrier(self.resource_root(copied->dst), Access::AccelerationStructureBuildWrite);
    } else if (opens_region(ir_node)) {
      const auto& declared = std::holds_alternative<ir::BeginRendering>(ir_node)
                               ? std::get<ir::BeginRendering>(ir_node).attachments
                               : std::get<ir::BeginCompute>(ir_node).attachments;
      if (const auto* rendering = std::get_if<ir::BeginRendering>(&ir_node)) {
        for (const auto& [cleared, color] : rendering->clears) {
          sync.discard(cleared);
        }
      }

      // descriptor images and declared images over the same range are moved to the state their
      // combined access needs in one transition
      auto image_uses = region_images.contains(id) ? region_images[id] : std::vector<RegionUse>{};
      auto buffer_uses = std::vector<RegionUse>{};
      for (const auto& [resource, access_id] : declared) {
        const auto access = resolve(access_id);
        for (const auto element : self.resource_elements(resource)) {
          if (self.is_buffer(element)) {
            buffer_uses.push_back({.resource = self.resource_root(element), .access = access});
          } else {
            merge_use(image_uses, element, access, true);
          }
        }
      }

      for (const auto& use : image_uses) {
        sync.transition(use.resource, sync.access_id(use.access), use.access);
      }

      for (const auto& use : buffer_uses) {
        sync.buffer_barrier(use.resource, use.access);
      }

      if (region_buffers.contains(id)) {
        for (const auto& use : region_buffers[id]) {
          sync.buffer_barrier(use.resource, use.access);
        }
      }
    } else if (auto* write = std::get_if<ir::WriteDescriptor>(&ir_node)) {
      if (const auto written = write->descriptor.image_value(); written.has_value()) {
        write->image_layout = sync.layout_of(*written);
      }
    } else if (const auto* dispatch = std::get_if<ir::Dispatch>(&ir_node)) {
      if (dispatch->size.kind == ir::DispatchSize::Kind::Indirect) {
        sync.buffer_barrier(self.resource_root(dispatch->size.resource), Access::IndirectRead);
      }
    } else if (const auto* indirect = std::get_if<ir::DrawIndirect>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(indirect->buffer), Access::IndirectRead);
      if (const auto counts = indirect->count.count_buffer(); counts.has_value()) {
        sync.buffer_barrier(self.resource_root(*counts), Access::IndirectRead);
      }
    } else if (const auto* mesh = std::get_if<ir::DrawMeshTasksIndirect>(&ir_node)) {
      sync.buffer_barrier(self.resource_root(mesh->buffer), Access::IndirectRead);
      if (const auto counts = mesh->count.count_buffer(); counts.has_value()) {
        sync.buffer_barrier(self.resource_root(*counts), Access::IndirectRead);
      }
    } else if (const auto* exported = std::get_if<ir::Export>(&ir_node)) {
      const auto access = resolve(exported->access);
      for (const auto element : self.resource_elements(exported->resource)) {
        if (self.is_buffer(element)) {
          sync.buffer_barrier(self.resource_root(element), access);
        } else {
          sync.transition(element, exported->access, access);
        }
      }
    }

    result.push_back(std::move(node));
  }

  return result;
}
} // namespace ox::vir
