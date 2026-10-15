#include "Render/Graph/Module.hpp"

#include <ankerl/svector.h>

#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto PipelineBindings::unchecked() -> PipelineBindings {
  return {
    .user = nullptr,
    .bindings_fn = [](const void*, PipelineID) -> option<std::span<const DescriptorBinding>> { return nullopt; },
    .bindless_set_fn = [](const void*, PipelineID) -> option<u32> { return nullopt; },
  };
}

auto PipelineBindings::bindings(this const PipelineBindings& self, PipelineID pipeline)
  -> option<std::span<const DescriptorBinding>> {
  return self.bindings_fn ? self.bindings_fn(self.user, pipeline) : nullopt;
}

auto PipelineBindings::bindless_set(this const PipelineBindings& self, PipelineID pipeline) -> option<u32> {
  return self.bindless_set_fn ? self.bindless_set_fn(self.user, pipeline) : nullopt;
}

auto Module::check_result_count(usize declared, usize expected) -> void {
  OX_ASSERT(declared == expected, "the pass declared %zu resources but its result expects %zu", declared, expected);
}

auto Module::get(this const Module& self, ValueID id) -> const IR& { return self.instructions[index_of(id)]; }

auto Module::emit(this Module& self, IR ir) -> ValueID {
  const auto id = static_cast<ValueID>(self.instructions.size());
  OX_ASSERT(is_valid(id), "exhausted valid value ids");

  const auto object = underlying_object(ir);
  const ResourceResolution* next = nullptr;
  if (object.kind == UnderlyingKind::Forwards || object.kind == UnderlyingKind::Element) {
    if (index_of(object.next) < self.resolutions.size()) {
      next = &self.resolutions[index_of(object.next)];
    }
  }

  auto resolution = resolution_for(id, ir, next);
  self.instructions.push_back(std::move(ir));
  self.resolutions.push_back(resolution);
  self.instruction_block.push_back(self.current_block);
  return id;
}

auto Module::lower_type(this Module& self, const ir::Type& type) -> ValueID {
  if (const auto it = self.types.find(type); it != self.types.end()) {
    return it->second;
  }

  const auto id = self.emit(type);
  self.types.emplace(type, id);
  return id;
}

auto Module::lower_constant(this Module& self, const ir::Constant& constant) -> ValueID {
  if (const auto it = self.constants.find(constant); it != self.constants.end()) {
    return it->second;
  }

  const auto id = self.emit(constant);
  self.constants.emplace(constant, id);
  return id;
}

auto Module::lower_u32(this Module& self, u32 value) -> ValueID {
  return self.lower_constant(ir::Constant::from_u32(value));
}

auto Module::lower_access(this Module& self, Access access) -> ValueID {
  return self.lower_constant(ir::Constant::from_access(access));
}

auto Module::lower_name(this Module& self, std::string_view name) -> ValueID {
  if (name.empty()) {
    return ValueID::Invalid;
  }

  return self.lower_constant(ir::Constant::from_string(std::string(name)));
}

auto Module::lower_count(this Module& self, Count count) -> ValueID {
  return is_valid(count.value) ? count.value : self.lower_u32(count.literal);
}

auto Module::resolve_access(this const Module& self, ValueID id) -> Access {
  const auto* constant = std::get_if<ir::Constant>(&self.get(id));
  OX_ASSERT(
    constant && constant->kind == ir::Constant::Kind::Access,
    "%%%u is not an access constant",
    std::to_underlying(id)
  );
  return constant->as_access();
}

auto Module::resource_root(this const Module& self, ValueID id) -> ValueID {
  if (index_of(id) >= self.resolutions.size()) {
    return id;
  }

  return self.resolutions[index_of(id)].root;
}

auto Module::resource_range(this const Module& self, ValueID id) -> Subresource {
  if (index_of(id) >= self.resolutions.size()) {
    return {};
  }

  return self.resolutions[index_of(id)].range;
}

auto Module::resource_elements(this const Module& self, ValueID id) -> std::vector<ValueID> {
  auto pending = std::vector<ValueID>{id};
  auto result = std::vector<ValueID>{};
  while (!pending.empty()) {
    const auto value = pending.back();
    pending.pop_back();
    const auto root = self.resource_root(value);
    const auto* array = index_of(root) < self.instructions.size() ? std::get_if<ir::Array>(&self.get(root)) : nullptr;
    if (array) {
      pending.insert(pending.end(), array->elements.rbegin(), array->elements.rend());
    } else {
      result.push_back(value);
    }
  }

  return result;
}

auto Module::resolve_resource(this const Module& self, ValueID id) -> const IR* {
  if (index_of(id) >= self.resolutions.size()) {
    return nullptr;
  }

  const auto base = self.resolutions[index_of(id)].base;
  if (!is_valid(base) || index_of(base) >= self.instructions.size()) {
    return nullptr;
  }

  return &self.get(base);
}

auto Module::is_buffer(this const Module& self, ValueID id) -> bool {
  const auto root = self.resource_root(id);
  return index_of(root) < self.instructions.size() && std::holds_alternative<ir::ConstructBuffer>(self.get(root));
}

auto Module::is_image(this const Module& self, ValueID id) -> bool {
  const auto* resource = self.resolve_resource(id);
  return resource && (std::holds_alternative<ir::ConstructImage>(*resource) ||
                      std::holds_alternative<ir::SwapchainImage>(*resource));
}

auto Module::image_format(this const Module& self, ValueID id) -> option<std::pair<Format, Samples>> {
  const auto* resource = self.resolve_resource(id);
  if (!resource) {
    return nullopt;
  }

  if (const auto* construct = std::get_if<ir::ConstructImage>(resource)) {
    if (index_of(id) < self.instructions.size()) {
      if (const auto* view = std::get_if<ir::View>(&self.get(id)); view && view->format != Format::Undefined) {
        return std::pair{view->format, construct->samples};
      }
    }

    return std::pair{construct->format, construct->samples};
  }

  if (const auto* swapchain = std::get_if<ir::SwapchainImage>(resource)) {
    return std::pair{swapchain->format, swapchain->samples};
  }

  return nullopt;
}

auto Module::image_extent(this const Module& self, ValueID id) -> option<Extent3D> {
  const auto* resource = self.resolve_resource(id);
  if (!resource) {
    return nullopt;
  }

  const auto* construct = std::get_if<ir::ConstructImage>(resource);
  if (!construct || !is_valid(construct->extent)) {
    return nullopt;
  }

  const auto* constant = std::get_if<ir::Constant>(&self.get(construct->extent));
  if (!constant || constant->kind != ir::Constant::Kind::Extent3D) {
    return nullopt;
  }

  // a view onto a lower mip sees the extent of that mip
  auto extent = constant->as_extent_3d();
  const auto level = self.resource_range(id).base_level;
  extent.width = std::max(extent.width >> level, 1_u32);
  extent.height = std::max(extent.height >> level, 1_u32);
  extent.depth = std::max(extent.depth >> level, 1_u32);
  return extent;
}

auto Module::buffer_size(this const Module& self, ValueID id) -> option<u64> {
  const auto* resource = self.resolve_resource(id);
  const auto* construct = resource ? std::get_if<ir::ConstructBuffer>(resource) : nullptr;
  if (!construct) {
    return nullopt;
  }

  if (!construct->buffer.is_null()) {
    return construct->buffer.size;
  }

  if (!is_valid(construct->size)) {
    return nullopt;
  }

  const auto* constant = std::get_if<ir::Constant>(&self.get(construct->size));
  if (!constant || constant->kind != ir::Constant::Kind::U64) {
    return nullopt;
  }

  return constant->as_u64();
}

auto Module::extent_2d(this const Module& self, ValueID id) -> option<Extent2D> {
  if (!is_valid(id)) {
    return nullopt;
  }

  const auto* constant = std::get_if<ir::Constant>(&self.get(id));
  if (!constant || constant->kind != ir::Constant::Kind::Extent2D) {
    return nullopt;
  }

  return constant->as_extent_2d();
}

auto Module::block_of(this const Module& self, ValueID id) -> LabelID {
  return index_of(id) < self.instruction_block.size() ? self.instruction_block[index_of(id)] : LabelID::Invalid;
}

auto Module::array_elements(this const Module& self, ValueID id) -> std::span<const ValueID> {
  const auto* array = std::get_if<ir::Array>(&self.get(id));
  OX_ASSERT(array != nullptr, "%%%u is not an array", std::to_underlying(id));
  return array->elements;
}

auto Module::resource_uses(this const Module& self, const IR& ir, std::vector<ValueID>& uses) -> void {
  visit_resource_side_effects(ir, [&](const ResourceSideEffect& effect) {
    for (const auto element : self.resource_elements(effect.resource)) {
      uses.push_back(self.resource_root(element));
    }
  });
}

auto Module::declare_label(this Module& self) -> LabelID { return static_cast<LabelID>(self.label_count++); }

auto Module::place_label(this Module& self, LabelID label) -> void {
  if (std::ranges::any_of(self.blocks, [&](const Block& block) { return block.label == label; })) {
    OX_LOG_ERROR("label {} is placed more than once", std::to_underlying(label));
    return;
  }

  self.blocks.push_back({.label = label});
  self.current_block = label;
}

auto Module::terminate(this Module& self, const Terminator& terminator) -> void {
  auto& block = self.blocks.back();
  if (block.terminator.kind != TerminatorKind::Return) {
    OX_LOG_ERROR("block {} is terminated twice", std::to_underlying(block.label));
    return;
  }

  block.terminator = terminator;
}

auto Module::open_selection(this Module& self, ValueID condition) -> std::array<LabelID, 3> {
  self.expect_no_open_pass("a selection is recorded");
  const auto merge = self.declare_label();
  const auto true_label = self.declare_label();
  const auto false_label = self.declare_label();
  self.terminate({
    .kind = TerminatorKind::BranchConditional,
    .condition = condition,
    .merge = merge,
    .true_label = true_label,
    .false_label = false_label,
  });
  self.place_label(true_label);
  return {merge, true_label, false_label};
}

auto Module::close_arm(this Module& self, LabelID merge) -> void {
  self.expect_no_open_pass("an arm ends");
  self.terminate({.kind = TerminatorKind::Branch, .target = merge});
}

auto Module::phi(this Module& self, std::initializer_list<std::pair<ValueID, LabelID>> incoming) -> ValueID {
  return self.emit(ir::Phi{.incoming = incoming});
}

auto Module::declare_var(this Module& self, ir::VariableKind kind, std::string_view name, Value initial) -> ValueID {
  const auto slot = static_cast<u32>(self.variables.size());
  const auto lowered_name = self.lower_name(name);
  const auto id = self.emit(ir::Variable{.slot = slot, .kind = kind, .name = lowered_name});
  self.variables.push_back({.kind = kind, .name = std::string(name), .value = std::move(initial)});
  self.variable_ids.push_back(id);
  return id;
}

auto Module::constant_u32(this Module& self, u32 value) -> ValueID { return self.lower_u32(value); }

auto Module::declare_bool_var(this Module& self, std::string_view name, bool initial) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::Bool}, name, initial);
}

auto Module::declare_i32_var(this Module& self, std::string_view name, i32 initial) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::I32}, name, initial);
}

auto Module::declare_u32_var(this Module& self, std::string_view name, u32 initial) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::U32}, name, initial);
}

auto Module::declare_extent_2d_var(this Module& self, std::string_view name, Extent2D initial) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::Extent2D}, name, initial);
}

auto Module::declare_extent_3d_var(this Module& self, std::string_view name, Extent3D initial) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::Extent3D}, name, initial);
}

auto Module::declare_clear_var(this Module& self, std::string_view name, const ClearValue& initial) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::ClearValue}, name, initial);
}

auto Module::declare_bytes_var(this Module& self, std::string_view name, u32 size) -> ValueID {
  return self
    .declare_var({.tag = ir::VariableKindTag::Bytes, .size = size}, name, Bytes{.data = std::vector<u8>(size, 0)});
}

auto Module::declare_callback_var(this Module& self, std::string_view name) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::Callback}, name, make_callback([](Recorder&) {}));
}

auto Module::declare_callback_var(this Module& self, std::string_view name, std::function<auto(Recorder&)->void> body)
  -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::Callback}, name, make_callback(std::move(body)));
}

auto Module::declare_swapchain_var(this Module& self, std::string_view name) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::Swapchain}, name, AcquiredSwapchain{});
}

auto Module::declare_acceleration_structure_builds_var(this Module& self, std::string_view name) -> ValueID {
  return self.declare_var(
    {.tag = ir::VariableKindTag::AccelerationStructureBuilds},
    name,
    std::make_shared<const std::vector<AccelerationStructureBuild>>()
  );
}

auto Module::declare_query_pool_var(this Module& self, std::string_view name) -> ValueID {
  return self.declare_var({.tag = ir::VariableKindTag::QueryPool}, name, QueryPool{});
}

auto Module::declare_buffer_var(this Module& self, std::string_view name, Access access) -> ValueID {
  self.lower_type({.kind = ir::TypeKind::Buffer});
  const auto initial_access = self.lower_access(access);
  const auto lowered_name = self.lower_name(name);
  const auto resource = self.emit(
    ir::ConstructBuffer{
      .location = MemoryLocation::Unknown,
      .initial_access = initial_access,
      .name = lowered_name,
    }
  );

  self.variables.push_back(
    {.kind = {.tag = ir::VariableKindTag::Buffer}, .name = std::string(name), .resource = resource}
  );
  self.variable_ids.push_back(resource);
  return resource;
}

auto Module::declare_image_var(this Module& self, std::string_view name, Format format, Access access, Samples samples)
  -> ValueID {
  const auto base_level = self.lower_u32(0);
  const auto level_count = self.lower_u32(REMAINING);
  const auto base_layer = self.lower_u32(0);
  const auto layer_count = self.lower_u32(REMAINING);
  self.lower_type({.kind = ir::TypeKind::Image, .format = format, .samples = samples});
  const auto lowered_name = self.lower_name(name);
  const auto initial_access = self.lower_access(access);
  const auto resource = self.emit(
    ir::ConstructImage{
      .format = format,
      .samples = samples,
      .base_level = base_level,
      .level_count = level_count,
      .base_layer = base_layer,
      .layer_count = layer_count,
      .initial_layout = to_image_layout(access),
      .initial_access = initial_access,
      .name = lowered_name,
    }
  );

  self.variables.push_back({
    .kind = {.tag = ir::VariableKindTag::ImageAttachment},
    .name = std::string(name),
    .resource = resource,
  });
  self.variable_ids.push_back(resource);
  return resource;
}

auto Module::transient_image(this Module& self, const ImageInfo& info) -> ValueID {
  const auto extent = self.lower_constant(ir::Constant::from_extent(info.extent));
  return self.transient_image_sized(info, extent);
}

auto Module::transient_image_sized(this Module& self, const ImageInfo& info, ValueID extent) -> ValueID {
  const auto base_level = self.lower_u32(0);
  const auto level_count = self.lower_u32(info.mip_levels);
  const auto base_layer = self.lower_u32(0);
  const auto layer_count = self.lower_u32(info.array_layers);
  const auto name = self.lower_name(info.name);
  const auto initial_access = self.lower_access(Access::None);
  self.lower_type({.kind = ir::TypeKind::Image, .format = info.format, .samples = info.samples});
  return self.emit(
    ir::ConstructImage{
      .view_type = info.default_view_type(),
      .image_type = info.image_type,
      .flags = info.flags,
      .extent = extent,
      .format = info.format,
      .samples = info.samples,
      .base_level = base_level,
      .level_count = level_count,
      .base_layer = base_layer,
      .layer_count = layer_count,
      .usage = info.usage,
      .view_formats = info.view_formats,
      .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
      .initial_access = initial_access,
      .name = name,
    }
  );
}

auto Module::import_image(this Module& self, const Image& image, VkImageLayout layout, Access access) -> ValueID {
  return self.import_attachment(ImageAttachment::from_image(image, layout), access);
}

auto Module::import_attachment(this Module& self, const ImageAttachment& attachment, Access access) -> ValueID {
  const auto extent = self.lower_constant(ir::Constant::from_extent(attachment.extent));
  const auto& range = attachment.subresource_range;
  const auto base_level = self.lower_u32(range.baseMipLevel);
  const auto level_count = self.lower_u32(range.levelCount);
  const auto base_layer = self.lower_u32(range.baseArrayLayer);
  const auto layer_count = self.lower_u32(range.layerCount);
  const auto initial_access = self.lower_access(access);
  self.lower_type({.kind = ir::TypeKind::Image, .format = attachment.format, .samples = attachment.samples});
  return self.emit(
    ir::ConstructImage{
      .image = attachment.image,
      .image_view = attachment.image_view,
      .view_type = attachment.view_type,
      .image_type = attachment.image.image_type,
      .extent = extent,
      .format = attachment.format,
      .samples = attachment.samples,
      .base_level = base_level,
      .level_count = level_count,
      .base_layer = base_layer,
      .layer_count = layer_count,
      .usage = attachment.image.usage,
      .initial_layout = attachment.layout,
      .initial_access = initial_access,
    }
  );
}

auto Module::transient_buffer(this Module& self, const BufferInfo& info) -> ValueID {
  const auto size = self.lower_constant(ir::Constant::from_u64(info.size));
  const auto initial_access = self.lower_access(Access::None);
  const auto name = self.lower_name(info.name);
  self.lower_type({.kind = ir::TypeKind::Buffer});
  return self.emit(
    ir::ConstructBuffer{
      .size = size,
      .usage = info.usage,
      .location = info.location,
      .initial_access = initial_access,
      .name = name,
    }
  );
}

auto Module::import_buffer(this Module& self, const Buffer& buffer, Access access) -> ValueID {
  self.lower_type({.kind = ir::TypeKind::Buffer});
  const auto size = self.lower_constant(ir::Constant::from_u64(buffer.size));
  const auto initial_access = self.lower_access(access);
  return self.emit(
    ir::ConstructBuffer{
      .buffer = buffer,
      .size = size,
      .location = MemoryLocation::Unknown,
      .initial_access = initial_access,
    }
  );
}

auto Module::set_name(this Module& self, ValueID id, std::string_view name) -> ValueID {
  auto& ir = self.instructions[index_of(id)];
  auto* slot = std::visit(
    [](auto& node) -> ValueID* {
      using T = std::decay_t<decltype(node)>;
      if constexpr (
        std::is_same_v<T, ir::ConstructImage> || std::is_same_v<T, ir::ConstructBuffer> ||
        std::is_same_v<T, ir::BeginRendering> || std::is_same_v<T, ir::BeginCompute>
      ) {
        return &node.name;
      } else {
        return nullptr;
      }
    },
    ir
  );
  if (!slot) {
    OX_LOG_WARN("%{} cannot carry a name, the name is dropped", std::to_underlying(id));
    return id;
  }

  const auto lowered = self.lower_name(name);
  // lowering may have grown the instruction list, so the slot is looked up again
  std::visit(
    [&](auto& node) {
      using T = std::decay_t<decltype(node)>;
      if constexpr (
        std::is_same_v<T, ir::ConstructImage> || std::is_same_v<T, ir::ConstructBuffer> ||
        std::is_same_v<T, ir::BeginRendering> || std::is_same_v<T, ir::BeginCompute>
      ) {
        node.name = lowered;
      }
    },
    self.instructions[index_of(id)]
  );
  return id;
}

auto Module::view(this Module& self, ValueID image, const Subresource& range, Format format, ImageViewType view_type)
  -> ValueID {
  if (!self.is_image(image)) {
    OX_LOG_ERROR("%{} is not an image to view", std::to_underlying(image));
    return image;
  }

  const auto root = self.resource_root(image);
  return self.emit(ir::View{.image = image, .root = root, .range = range, .format = format, .view_type = view_type});
}

auto Module::mip(this Module& self, ValueID image, u32 level) -> ValueID {
  return self.view(image, Subresource::mip(level));
}

auto Module::subrange(this Module& self, ValueID image, u32 base_mip, u32 mip_count, u32 base_layer, u32 layer_count)
  -> ValueID {
  return self.view(
    image,
    {.base_level = base_mip, .level_count = mip_count, .base_layer = base_layer, .layer_count = layer_count}
  );
}

auto Module::whole(this Module& self, ValueID image) -> ValueID { return self.view(image, Subresource::whole()); }

auto Module::generate_mips(this Module& self, ValueID image, u32 base_level, u32 level_count) -> ValueID {
  auto current = image;
  for (u32 level = base_level + 1; level < base_level + level_count; level++) {
    const auto src = self.view(current, Subresource::mip(level - 1));
    const auto dst = self.view(current, Subresource::mip(level));
    current = self.blit(src, dst, VK_FILTER_LINEAR);
  }

  return self.whole(current);
}

auto Module::acquire_next_image(this Module& self, const SwapChain& swapchain) -> ValueID {
  const auto variable = self.declare_swapchain_var("swapchain");
  self.variables.back().value = AcquiredSwapchain::from(swapchain);
  return self.acquire_next_image_from(variable, swapchain.format(), swapchain.samples());
}

auto Module::acquire_next_image_from(this Module& self, ValueID swapchain, Format format, Samples samples) -> ValueID {
  self.lower_type({.kind = ir::TypeKind::Image, .format = format, .samples = samples});
  const auto acquire = self.emit(ir::AcquireNextImage{.swapchain = swapchain});
  return self.emit(
    ir::SwapchainImage{.swapchain = swapchain, .acquire = acquire, .format = format, .samples = samples}
  );
}

auto Module::export_to(this Module& self, ValueID resource, Access access, DomainFlag dst_domain) -> ValueID {
  const auto lowered = self.lower_access(access);
  return self.emit(ir::Export{.resource = resource, .access = lowered, .dst_domain = dst_domain});
}

auto Module::present(this Module& self, ValueID attachment) -> ValueID {
  return self.export_to(attachment, Access::Present, DomainFlag::Present);
}

auto Module::keep(this Module& self, ValueID value) -> ValueID {
  self.kept.push_back(value);
  return value;
}

auto Module::clear(this Module& self, ValueID attachment, const ClearValue& color) -> ValueID {
  const auto lowered = self.lower_constant(ir::Constant::from_clear(color));
  return self.clear_from(attachment, lowered);
}

auto Module::clear_from(this Module& self, ValueID attachment, ValueID color) -> ValueID {
  return self.emit(ir::Clear{.attachment = attachment, .color = color});
}

auto Module::blit(this Module& self, ValueID src, ValueID dst, VkFilter filter) -> ValueID {
  return self.emit(ir::Blit{.src = src, .dst = dst, .filter = filter});
}

auto Module::copy_buffer_to_image(this Module& self, ValueID buffer, ValueID image) -> ValueID {
  // an image bound to a variable has no extent here, the interpreter measures off what was bound
  return self.emit(ir::CopyBufferToImage{.buffer = buffer, .image = image});
}

auto Module::copy_buffer_to_image_region(
  this Module& self, ValueID buffer, ValueID image, const BufferImageCopy& region
) -> ValueID {
  if (region.is_empty()) {
    OX_LOG_WARN("copy region into %{} is empty, the copy is dropped", std::to_underlying(image));
    return image;
  }

  return self.emit(ir::CopyBufferToImage{.buffer = buffer, .image = image, .region = region});
}

auto Module::copy_image_to_buffer(this Module& self, ValueID image, ValueID buffer) -> ValueID {
  return self.emit(ir::CopyImageToBuffer{.image = image, .buffer = buffer});
}

auto Module::copy_image_to_buffer_region(
  this Module& self, ValueID image, ValueID buffer, const BufferImageCopy& region
) -> ValueID {
  if (region.is_empty()) {
    OX_LOG_WARN("copy region out of %{} is empty, the copy is dropped", std::to_underlying(image));
    return buffer;
  }

  return self.emit(ir::CopyImageToBuffer{.image = image, .buffer = buffer, .region = region});
}

auto Module::copy_buffer(this Module& self, ValueID src, ValueID dst, const BufferCopy& region) -> ValueID {
  if (!self.is_buffer(src) || !self.is_buffer(dst)) {
    OX_LOG_ERROR("a buffer copy can only go between buffers, the copy is dropped");
    return dst;
  }

  return self.emit(ir::CopyBuffer{.src = src, .dst = dst, .region = region});
}

auto Module::fill_buffer(this Module& self, ValueID buffer, u32 data, u64 offset, u64 size) -> ValueID {
  if (!self.is_buffer(buffer)) {
    OX_LOG_ERROR("only a buffer can be filled, the fill is dropped");
    return buffer;
  }

  return self.emit(ir::FillBuffer{.buffer = buffer, .offset = offset, .size = size, .data = data});
}

auto Module::copy_image(this Module& self, ValueID src, ValueID dst) -> ValueID {
  return self.emit(ir::CopyImage{.src = src, .dst = dst});
}

auto Module::update_buffer(this Module& self, ValueID buffer, std::span<const u8> data, u64 offset) -> ValueID {
  self.expect_no_open_pass("a buffer update is recorded");
  if (data.empty()) {
    return buffer;
  }
  if (!self.is_buffer(buffer) || offset % 4 != 0 || data.size() % 4 != 0 || data.size() > 65536) {
    OX_LOG_ERROR("a buffer update needs a buffer and up to 64 KiB of aligned data, the update is dropped");
    return buffer;
  }
  return self.emit(ir::UpdateBuffer{.buffer = buffer, .data = {data.begin(), data.end()}, .offset = offset});
}

auto Module::update_buffer_from(this Module& self, ValueID buffer, ValueID source, u64 offset) -> ValueID {
  self.expect_no_open_pass("a buffer update is recorded");
  const auto* variable = std::get_if<ir::Variable>(&self.get(source));
  if (
    !self.is_buffer(buffer) || offset % 4 != 0 || !variable || variable->kind.tag != ir::VariableKindTag::Bytes ||
    variable->kind.size == 0 || variable->kind.size % 4 != 0 || variable->kind.size > 65536
  ) {
    OX_LOG_ERROR("a buffer update source must be an aligned bytes variable of up to 64 KiB, the update is dropped");
    return buffer;
  }
  return self.emit(ir::UpdateBuffer{.buffer = buffer, .source = source, .offset = offset});
}

auto Module::copy_image_region(this Module& self, ValueID src, ValueID dst, const ImageCopy& region) -> ValueID {
  self.expect_no_open_pass("an image copy is recorded");
  if (region.is_empty()) {
    return dst;
  }
  if (!self.is_image(src) || !self.is_image(dst)) {
    OX_LOG_ERROR("an image copy can only go between images, the copy is dropped");
    return dst;
  }
  return self.emit(ir::CopyImage{.src = src, .dst = dst, .region = region});
}

auto Module::resolve(this Module& self, ValueID src, ValueID dst) -> ValueID {
  self.expect_no_open_pass("an image resolve is recorded");
  if (!self.is_image(src) || !self.is_image(dst)) {
    OX_LOG_ERROR("an image resolve can only go between images, the resolve is dropped");
    return dst;
  }
  return self.emit(ir::ResolveImage{.src = src, .dst = dst});
}

auto Module::resolve_region(this Module& self, ValueID src, ValueID dst, const ImageCopy& region) -> ValueID {
  self.expect_no_open_pass("an image resolve is recorded");
  if (region.is_empty()) {
    return dst;
  }
  if (!self.is_image(src) || !self.is_image(dst)) {
    OX_LOG_ERROR("an image resolve can only go between images, the resolve is dropped");
    return dst;
  }
  return self.emit(ir::ResolveImage{.src = src, .dst = dst, .region = region});
}

auto Module::lower_attachments(this Module& self, std::span<const ResourceAccess> attachments)
  -> std::vector<std::pair<ValueID, ValueID>> {
  auto merged = std::vector<ResourceAccess>{};
  for (const auto& attachment : attachments) {
    auto it = std::ranges::find(merged, attachment.resource, &ResourceAccess::resource);
    if (it != merged.end()) {
      it->access |= attachment.access;
    } else {
      merged.push_back(attachment);
    }
  }

  auto lowered = std::vector<std::pair<ValueID, ValueID>>{};
  for (const auto& attachment : merged) {
    lowered.emplace_back(attachment.resource, self.lower_access(attachment.access));
  }

  return lowered;
}

auto Module::transient_query_pool(this Module& self, VkQueryType type, u32 count) -> ValueID {
  OX_ASSERT(count != 0, "a query pool cannot be empty");
  return self.emit(ir::ConstructQueryPool{.type = type, .count = count});
}

auto Module::build_acceleration_structures(
  this Module& self,
  ValueID builds,
  std::span<const AccelerationStructureAddress> addresses,
  std::span<const ResourceAccess> resources
) -> std::vector<ValueID> {
  self.expect_no_open_pass("an acceleration structure build is recorded");
  const auto* variable = std::get_if<ir::Variable>(&self.get(builds));
  auto uses = ankerl::svector<ResourceAccess, 8>(resources.begin(), resources.end());
  for (const auto& address : addresses) {
    uses.push_back(
      {address.buffer,
       address.kind == AccelerationStructureAddressKind::Scratch ? Access::AccelerationStructureBuildRW
                                                                 : Access::AccelerationStructureBuildRead}
    );
  }
  if (
    !variable || variable->kind.tag != ir::VariableKindTag::AccelerationStructureBuilds || resources.empty() ||
    std::ranges::any_of(uses, [&](const ResourceAccess& use) { return !self.is_buffer(use.resource); })
  ) {
    OX_LOG_ERROR("acceleration structure builds need a builds variable and declared buffers, the builds are dropped");
    auto unchanged = std::vector<ValueID>{};
    for (const auto& resource : resources) {
      unchanged.push_back(resource.resource);
    }
    return unchanged;
  }
  auto lowered = self.lower_attachments(uses);
  const auto completion = self.emit(
    ir::BuildAccelerationStructures{
      .builds = builds,
      .addresses = {addresses.begin(), addresses.end()},
      .resources = std::move(lowered)
    }
  );
  auto results = std::vector<ValueID>{};
  for (const auto& resource : resources) {
    results.push_back(self.emit(ir::PassResult{.completion = completion, .resource = resource.resource}));
  }
  return results;
}

auto Module::write_acceleration_structure_properties(
  this Module& self, ValueID source, VkAccelerationStructureKHR structure, ValueID queries, u32 first_query
) -> ValueID {
  self.expect_no_open_pass("an acceleration structure query is recorded");
  const auto* variable = std::get_if<ir::Variable>(&self.get(queries));
  if (
    !self.is_buffer(source) || structure == VK_NULL_HANDLE ||
    (!std::holds_alternative<ir::ConstructQueryPool>(self.get(queries)) &&
     (!variable || variable->kind.tag != ir::VariableKindTag::QueryPool))
  ) {
    OX_LOG_ERROR("an acceleration structure query needs a backing buffer, handle and query pool, the query is dropped");
    return source;
  }
  return self.emit(
    ir::WriteAccelerationStructureProperties{
      .source = source,
      .structure = structure,
      .queries = queries,
      .first_query = first_query
    }
  );
}

auto Module::copy_acceleration_structure(
  this Module& self,
  ValueID src,
  ValueID dst,
  VkAccelerationStructureKHR source,
  VkAccelerationStructureKHR destination,
  VkCopyAccelerationStructureModeKHR mode
) -> ValueID {
  self.expect_no_open_pass("an acceleration structure copy is recorded");
  if (
    !self.is_buffer(src) || !self.is_buffer(dst) || source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE ||
    (mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR && mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR)
  ) {
    OX_LOG_ERROR("an acceleration structure copy needs backing buffers and handles, the copy is dropped");
    return dst;
  }
  return self.emit(
    ir::CopyAccelerationStructure{.src = src, .dst = dst, .source = source, .destination = destination, .mode = mode}
  );
}

auto Module::begin_rendering(this Module& self, std::initializer_list<ResourceAccess> attachments) -> Module& {
  return self.begin_rendering(std::span(attachments.begin(), attachments.size()));
}

auto Module::begin_rendering(this Module& self, std::span<const ResourceAccess> attachments) -> Module& {
  OX_ASSERT(!attachments.empty(), "a rendering pass must declare at least one resource");
  auto resources = std::vector<ValueID>{};
  for (const auto& attachment : attachments) {
    resources.push_back(attachment.resource);
  }

  auto lowered = self.lower_attachments(attachments);
  const auto id = self.emit(ir::BeginRendering{.attachments = std::move(lowered)});
  return self.open_pass(PassKind::Rendering, id, std::move(resources));
}

auto Module::begin_rendering_area(
  this Module& self,
  std::initializer_list<ResourceAccess> attachments,
  Extent2D render_area,
  u32 layer_count,
  u32 view_mask
) -> Module& {
  return self
    .begin_rendering_area(std::span(attachments.begin(), attachments.size()), render_area, layer_count, view_mask);
}

auto Module::begin_rendering_area(
  this Module& self, std::span<const ResourceAccess> attachments, Extent2D render_area, u32 layer_count, u32 view_mask
) -> Module& {
  OX_ASSERT(render_area.width != 0 && render_area.height != 0 && layer_count != 0, "a rendering area cannot be empty");
  auto resources = std::vector<ValueID>{};
  for (const auto& attachment : attachments) {
    resources.push_back(attachment.resource);
  }

  const auto area = self.lower_constant(ir::Constant::from_extent(render_area));
  auto lowered = self.lower_attachments(attachments);
  const auto id = self.emit(
    ir::BeginRendering{
      .attachments = std::move(lowered),
      .render_area = area,
      .layer_count = layer_count,
      .view_mask = view_mask,
    }
  );
  return self.open_pass(PassKind::Rendering, id, std::move(resources));
}

auto Module::begin_compute(this Module& self, std::initializer_list<ResourceAccess> attachments) -> Module& {
  return self.begin_compute(std::span(attachments.begin(), attachments.size()));
}

auto Module::begin_compute(this Module& self, std::span<const ResourceAccess> attachments) -> Module& {
  OX_ASSERT(!attachments.empty(), "a compute pass must declare at least one resource");
  auto resources = std::vector<ValueID>{};
  for (const auto& attachment : attachments) {
    resources.push_back(attachment.resource);
  }

  auto lowered = self.lower_attachments(attachments);
  const auto id = self.emit(ir::BeginCompute{.attachments = std::move(lowered)});
  return self.open_pass(PassKind::Compute, id, std::move(resources));
}

auto Module::open_pass(this Module& self, PassKind kind, ValueID begin, std::vector<ValueID> resources) -> Module& {
  self.expect_no_open_pass("another one opens");
  self.ongoing_pass = OngoingPass{.kind = kind, .begin = begin, .tip = begin, .resources = std::move(resources)};
  return self;
}

auto Module::expect_no_open_pass(this Module& self, std::string_view when) -> void {
  if (self.ongoing_pass.has_value()) {
    OX_LOG_ERROR(
      "a pass opened at %{} is still open where {}, it is closed here",
      std::to_underlying(self.ongoing_pass->begin),
      when
    );
    self.ongoing_pass.reset();
  }
}

auto Module::chain(this Module& self, option<PassKind> wanted, std::string_view what, IR ir) -> Module& {
  if (!self.ongoing_pass.has_value()) {
    OX_LOG_ERROR("{} is recorded outside of a pass, it is dropped", what);
    return self;
  }

  if (wanted.has_value() && self.ongoing_pass->kind != *wanted) {
    OX_LOG_ERROR("{} belongs to another kind of pass than the one open, it is dropped", what);
    return self;
  }

  const auto tip = self.ongoing_pass->tip;
  std::visit(
    [&](auto& node) {
      if constexpr (requires { node.pass; }) {
        node.pass = tip;
      }
    },
    ir
  );
  const auto id = self.emit(std::move(ir));
  self.ongoing_pass->tip = id;
  return self;
}

auto Module::set_state(this Module& self, StateChange change) -> Module& {
  return self.chain(PassKind::Rendering, "state", ir::SetState{.change = std::move(change)});
}

auto Module::pass_state(this Module& self, std::string_view what, StateChange change) -> Module& {
  return self.chain(nullopt, what, ir::SetState{.change = std::move(change)});
}

auto Module::with_name(this Module& self, std::string_view name) -> Module& {
  if (!self.ongoing_pass.has_value()) {
    OX_LOG_ERROR("no pass is open to name, the name is dropped");
    return self;
  }

  self.set_name(self.ongoing_pass->begin, name);
  return self;
}

auto Module::push_constant_bytes(this Module& self, u32 offset, std::span<const u8> data) -> Module& {
  return self.pass_state("push constants", state::Push{.offset = offset, .data = {data.begin(), data.end()}});
}

auto Module::push_constants_from(this Module& self, ValueID variable) -> Module& {
  return self.push_constants_from_at(0, variable);
}

auto Module::push_constants_from_at(this Module& self, u32 offset, ValueID variable) -> Module& {
  const auto* declared = std::get_if<ir::Variable>(&self.get(variable));
  if (!declared || declared->kind.tag != ir::VariableKindTag::Bytes) {
    OX_LOG_ERROR("push constants can only come from a bytes variable");
    return self;
  }

  return self.pass_state(
    "push constants",
    state::PushFrom{.offset = offset, .size = declared->kind.size, .source = variable}
  );
}

auto Module::push_constant_address(this Module& self, u32 offset, ValueID buffer) -> Module& {
  return self.pass_state("push constants", state::PushAddress{.offset = offset, .buffer = buffer});
}

auto Module::specialize(this Module& self, u32 id, SpecValue value) -> Module& {
  return self.pass_state("specialization constant", state::SpecializationConstant{.id = id, .value = value});
}

auto Module::bind_graphics_pipeline(this Module& self, PipelineID pipeline) -> Module& {
  return self.chain(
    PassKind::Rendering,
    "pipeline",
    ir::BindPipeline{.pipeline = pipeline, .bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS}
  );
}

auto Module::bind_compute_pipeline(this Module& self, PipelineID pipeline) -> Module& {
  return self.chain(
    PassKind::Compute,
    "pipeline",
    ir::BindPipeline{.pipeline = pipeline, .bind_point = VK_PIPELINE_BIND_POINT_COMPUTE}
  );
}

auto Module::bind_ray_tracing_pipeline(this Module& self, PipelineID pipeline) -> Module& {
  return self.chain(
    PassKind::Compute,
    "pipeline",
    ir::BindPipeline{.pipeline = pipeline, .bind_point = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR}
  );
}

auto Module::write_descriptor(this Module& self, u32 set, u32 binding, const ir::Descriptor& descriptor) -> Module& {
  if (const auto image = descriptor.image_value(); image.has_value() && !self.is_image(*image)) {
    OX_LOG_ERROR("only an image can be written to the image descriptor at set {} binding {}", set, binding);
    return self;
  }

  if (const auto buffer = descriptor.buffer_value(); buffer.has_value() && !self.is_buffer(*buffer)) {
    OX_LOG_ERROR("only a buffer can be written to the buffer descriptor at set {} binding {}", set, binding);
    return self;
  }

  const auto access = self.lower_access(Access::None);
  return self.chain(
    nullopt,
    "descriptor",
    ir::WriteDescriptor{.set = set, .binding = binding, .descriptor = descriptor, .access = access}
  );
}

auto Module::bind_image(this Module& self, u32 set, u32 binding, ValueID image) -> Module& {
  return self.write_descriptor(set, binding, {.kind = ir::Descriptor::Kind::Image, .image = image});
}

auto Module::bind_image(this Module& self, u32 set, u32 binding, ValueID image, Format view_format) -> Module& {
  const auto viewed = self.view(image, self.resource_range(image), view_format);
  return self.bind_image(set, binding, viewed);
}

auto Module::bind_texture(this Module& self, u32 set, u32 binding, ValueID image, VkSampler sampler) -> Module& {
  return self.write_descriptor(
    set,
    binding,
    {.kind = ir::Descriptor::Kind::CombinedImageSampler, .image = image, .sampler = sampler}
  );
}

auto Module::bind_sampler(this Module& self, u32 set, u32 binding, VkSampler sampler) -> Module& {
  return self.write_descriptor(set, binding, {.kind = ir::Descriptor::Kind::Sampler, .sampler = sampler});
}

auto Module::bind_buffer(this Module& self, u32 set, u32 binding, ValueID buffer) -> Module& {
  return self.bind_buffer_range(set, binding, buffer, 0, VK_WHOLE_SIZE);
}

auto Module::bind_buffer_range(this Module& self, u32 set, u32 binding, ValueID buffer, u64 offset, u64 range)
  -> Module& {
  return self.write_descriptor(
    set,
    binding,
    {.kind = ir::Descriptor::Kind::Buffer, .buffer = buffer, .offset = offset, .range = range}
  );
}

auto Module::bind_texel_buffer(this Module& self, u32 set, u32 binding, ValueID buffer, VkBufferView view) -> Module& {
  return self
    .write_descriptor(set, binding, {.kind = ir::Descriptor::Kind::TexelBuffer, .buffer = buffer, .view = view});
}

auto Module::bind_acceleration_structure(
  this Module& self, u32 set, u32 binding, ValueID backing_buffer, VkAccelerationStructureKHR acceleration_structure
) -> Module& {
  return self.write_descriptor(
    set,
    binding,
    {
      .kind = ir::Descriptor::Kind::AccelerationStructure,
      .buffer = backing_buffer,
      .acceleration_structure = acceleration_structure,
    }
  );
}

auto Module::bind_vertex_buffer(this Module& self, u32 binding, ValueID buffer, u64 offset) -> Module& {
  const ValueID buffers[] = {buffer};
  const u64 offsets[] = {offset};
  return self.bind_vertex_buffers(binding, buffers, offsets);
}

auto Module::bind_vertex_buffers(
  this Module& self, u32 first_binding, std::span<const ValueID> buffers, std::span<const u64> offsets
) -> Module& {
  OX_ASSERT(buffers.size() == offsets.size(), "every bound vertex buffer needs an offset");
  const auto buffer_type = self.lower_type({.kind = ir::TypeKind::Buffer});
  const auto buffer_array = self.emit(ir::Array{.ty = buffer_type, .elements = {buffers.begin(), buffers.end()}});
  const auto offset_type = self.lower_type({.kind = ir::TypeKind::U64});
  auto offset_elements = std::vector<ValueID>{};
  for (const auto offset : offsets) {
    offset_elements.push_back(self.lower_constant(ir::Constant::from_u64(offset)));
  }

  const auto offset_array = self.emit(ir::Array{.ty = offset_type, .elements = std::move(offset_elements)});
  return self.chain(
    PassKind::Rendering,
    "vertex buffers",
    ir::BindVertexBuffers{.first_binding = first_binding, .buffers = buffer_array, .offsets = offset_array}
  );
}

auto Module::bind_index_buffer(this Module& self, ValueID buffer, VkIndexType index_type, u64 offset) -> Module& {
  return self.chain(
    PassKind::Rendering,
    "index buffer",
    ir::BindIndexBuffer{.buffer = buffer, .offset = offset, .index_type = index_type}
  );
}

auto Module::set_primitive_topology(this Module& self, VkPrimitiveTopology topology) -> Module& {
  return self.set_state(state::PrimitiveTopology{.topology = topology});
}

auto Module::set_patch_control_points(this Module& self, u32 count) -> Module& {
  return self.set_state(state::PatchControlPoints{.count = count});
}

auto Module::set_depth(this Module& self, const DepthState& depth) -> Module& {
  return self.set_state(state::Depth{.depth = depth});
}

auto Module::set_stencil(this Module& self, const StencilState& stencil) -> Module& {
  return self.set_state(state::Stencil{.stencil = stencil});
}

auto Module::set_rasterization(this Module& self, const RasterizationState& rasterization) -> Module& {
  return self.set_state(state::Rasterization{.rasterization = rasterization});
}

auto Module::set_dynamic_state(this Module& self, DynamicStateFlags dynamic) -> Module& {
  return self.set_state(state::Dynamic{.flags = dynamic});
}

auto Module::set_viewport(this Module& self, u32 index, const Viewport& viewport) -> Module& {
  return self.set_state(state::SetViewport{.index = index, .viewport = viewport});
}

auto Module::set_viewport(this Module& self, u32 index, const Rect2D& rect) -> Module& {
  return self.set_viewport(index, Viewport{.rect = rect});
}

auto Module::set_scissor(this Module& self, u32 index, const Rect2D& rect) -> Module& {
  return self.set_state(state::SetScissor{.index = index, .rect = rect});
}

auto Module::set_color_blend(this Module& self, u32 index, const ColorBlendAttachmentState& blend) -> Module& {
  return self.set_state(state::ColorBlend{.index = index, .blend = blend});
}

auto Module::set_color_blend(this Module& self, u32 index, BlendPreset preset) -> Module& {
  return self.set_color_blend(index, blend_preset(preset));
}

auto Module::broadcast_color_blend(this Module& self, const ColorBlendAttachmentState& blend) -> Module& {
  return self.set_state(state::ColorBlend{.index = nullopt, .blend = blend});
}

auto Module::broadcast_color_blend(this Module& self, BlendPreset preset) -> Module& {
  return self.broadcast_color_blend(blend_preset(preset));
}

auto Module::draw(this Module& self, Count vertex_count, Count instance_count) -> Module& {
  return self.draw_range(vertex_count, instance_count, 0_u32, 0_u32);
}

auto Module::draw_range(
  this Module& self, Count vertex_count, Count instance_count, Count first_vertex, Count first_instance
) -> Module& {
  auto draw = ir::Draw{
    .vertex_count = self.lower_count(vertex_count),
    .instance_count = self.lower_count(instance_count),
    .first_vertex = self.lower_count(first_vertex),
    .first_instance = self.lower_count(first_instance),
  };
  return self.chain(PassKind::Rendering, "draw", std::move(draw));
}

auto Module::draw_indexed(this Module& self, Count index_count, Count instance_count) -> Module& {
  return self.draw_indexed_range(index_count, instance_count, 0_u32, 0, 0_u32);
}

auto Module::draw_indexed_range(
  this Module& self, Count index_count, Count instance_count, Count first_index, i32 vertex_offset, Count first_instance
) -> Module& {
  auto draw = ir::DrawIndexed{
    .index_count = self.lower_count(index_count),
    .instance_count = self.lower_count(instance_count),
    .first_index = self.lower_count(first_index),
    .vertex_offset = self.lower_constant(ir::Constant::from_i32(vertex_offset)),
    .first_instance = self.lower_count(first_instance),
  };
  return self.chain(PassKind::Rendering, "indexed draw", std::move(draw));
}

auto Module::draw_indirect(this Module& self, ValueID buffer, Count draw_count, u64 offset, u32 stride) -> Module& {
  if (!self.is_buffer(buffer)) {
    OX_LOG_ERROR("an indirect draw can only read its commands out of a buffer");
    return self;
  }

  auto draw = ir::DrawIndirect{
    .buffer = buffer,
    .offset = offset,
    .stride = stride,
    .count = {.fixed = self.lower_count(draw_count)},
  };
  return self.chain(PassKind::Rendering, "indirect draw", std::move(draw));
}

auto Module::draw_indexed_indirect(this Module& self, ValueID buffer, Count draw_count, u64 offset, u32 stride)
  -> Module& {
  if (!self.is_buffer(buffer)) {
    OX_LOG_ERROR("an indirect draw can only read its commands out of a buffer");
    return self;
  }

  auto draw = ir::DrawIndirect{
    .is_indexed = true,
    .buffer = buffer,
    .offset = offset,
    .stride = stride,
    .count = {.fixed = self.lower_count(draw_count)},
  };
  return self.chain(PassKind::Rendering, "indirect draw", std::move(draw));
}

auto Module::draw_indirect_count(
  this Module& self,
  ValueID buffer,
  u64 offset,
  ValueID count_buffer,
  u64 count_offset,
  Count max_draw_count,
  u32 stride
) -> Module& {
  if (!self.is_buffer(buffer) || !self.is_buffer(count_buffer)) {
    OX_LOG_ERROR("an indirect draw can only read its commands and count out of buffers");
    return self;
  }

  auto draw = ir::DrawIndirect{
    .buffer = buffer,
    .offset = offset,
    .stride = stride,
    .count = {.buffer = count_buffer, .offset = count_offset, .max = self.lower_count(max_draw_count)},
  };
  return self.chain(PassKind::Rendering, "indirect draw", std::move(draw));
}

auto Module::draw_indexed_indirect_count(
  this Module& self,
  ValueID buffer,
  u64 offset,
  ValueID count_buffer,
  u64 count_offset,
  Count max_draw_count,
  u32 stride
) -> Module& {
  if (!self.is_buffer(buffer) || !self.is_buffer(count_buffer)) {
    OX_LOG_ERROR("an indirect draw can only read its commands and count out of buffers");
    return self;
  }

  auto draw = ir::DrawIndirect{
    .is_indexed = true,
    .buffer = buffer,
    .offset = offset,
    .stride = stride,
    .count = {.buffer = count_buffer, .offset = count_offset, .max = self.lower_count(max_draw_count)},
  };
  return self.chain(PassKind::Rendering, "indirect draw", std::move(draw));
}

auto Module::draw_mesh_tasks(this Module& self, Count x, Count y, Count z) -> Module& {
  auto draw = ir::DrawMeshTasks{.x = self.lower_count(x), .y = self.lower_count(y), .z = self.lower_count(z)};
  return self.chain(PassKind::Rendering, "mesh tasks", std::move(draw));
}

auto Module::draw_mesh_tasks_indirect(this Module& self, ValueID buffer, Count draw_count, u64 offset, u32 stride)
  -> Module& {
  if (!self.is_buffer(buffer)) {
    OX_LOG_ERROR("an indirect draw can only read its commands out of a buffer");
    return self;
  }

  auto draw = ir::DrawMeshTasksIndirect{
    .buffer = buffer,
    .offset = offset,
    .stride = stride,
    .count = {.fixed = self.lower_count(draw_count)},
  };
  return self.chain(PassKind::Rendering, "indirect mesh tasks", std::move(draw));
}

auto Module::draw_mesh_tasks_indirect_count(
  this Module& self,
  ValueID buffer,
  u64 offset,
  ValueID count_buffer,
  u64 count_offset,
  Count max_draw_count,
  u32 stride
) -> Module& {
  if (!self.is_buffer(buffer) || !self.is_buffer(count_buffer)) {
    OX_LOG_ERROR("an indirect draw can only read its commands and count out of buffers");
    return self;
  }

  auto draw = ir::DrawMeshTasksIndirect{
    .buffer = buffer,
    .offset = offset,
    .stride = stride,
    .count = {.buffer = count_buffer, .offset = count_offset, .max = self.lower_count(max_draw_count)},
  };
  return self.chain(PassKind::Rendering, "indirect mesh tasks", std::move(draw));
}

auto Module::record_from(this Module& self, ValueID body) -> Module& {
  return self.chain(nullopt, "callback", ir::CallOpaque{.body = body});
}

auto Module::dispatch(this Module& self, Count groups_x, Count groups_y, Count groups_z) -> Module& {
  auto size = ir::DispatchSize{
    .kind = ir::DispatchSize::Kind::Groups,
    .x = self.lower_count(groups_x),
    .y = self.lower_count(groups_y),
    .z = self.lower_count(groups_z),
  };
  return self.chain(PassKind::Compute, "dispatch", ir::Dispatch{.size = size});
}

auto Module::dispatch_invocations(this Module& self, Count x, Count y, Count z) -> Module& {
  auto size = ir::DispatchSize{
    .kind = ir::DispatchSize::Kind::Invocations,
    .x = self.lower_count(x),
    .y = self.lower_count(y),
    .z = self.lower_count(z),
  };
  return self.chain(PassKind::Compute, "dispatch", ir::Dispatch{.size = size});
}

auto Module::dispatch_invocations_per_pixel(this Module& self, ValueID image, std::array<f32, 3> scale) -> Module& {
  if (const auto extent = self.image_extent(image); extent.has_value()) {
    return self.dispatch_invocations(
      ir::scaled(extent->width, scale[0]),
      ir::scaled(extent->height, scale[1]),
      ir::scaled(extent->depth, scale[2])
    );
  }

  // an image sized at run time is counted when the module runs
  if (!self.is_image(image)) {
    OX_LOG_WARN("cannot infer the extent to dispatch over, the dispatch is dropped");
    return self;
  }

  auto size = ir::DispatchSize{.kind = ir::DispatchSize::Kind::InvocationsPerPixel, .resource = image, .scale = scale};
  return self.chain(PassKind::Compute, "dispatch", ir::Dispatch{.size = size});
}

auto Module::dispatch_invocations_per_element(this Module& self, ValueID buffer, u64 element_size, f32 scale)
  -> Module& {
  if (element_size == 0) {
    OX_LOG_WARN("elements of no size have no count to dispatch over, the dispatch is dropped");
    return self;
  }

  if (const auto size = self.buffer_size(buffer); size.has_value()) {
    return self.dispatch_invocations(ir::scaled(*size / element_size, scale), 1_u32, 1_u32);
  }

  if (!self.is_buffer(buffer)) {
    OX_LOG_WARN("cannot infer the element count to dispatch over, the dispatch is dropped");
    return self;
  }

  auto size = ir::DispatchSize{
    .kind = ir::DispatchSize::Kind::InvocationsPerElement,
    .resource = buffer,
    .scale = {scale, 1.0f, 1.0f},
    .element_size = element_size,
  };
  return self.chain(PassKind::Compute, "dispatch", ir::Dispatch{.size = size});
}

auto Module::dispatch_indirect(this Module& self, ValueID buffer, u64 offset) -> Module& {
  auto size = ir::DispatchSize{.kind = ir::DispatchSize::Kind::Indirect, .resource = buffer, .offset = offset};
  return self.chain(PassKind::Compute, "dispatch", ir::Dispatch{.size = size});
}

auto Module::trace_rays(this Module& self, Count width, Count height, Count depth) -> Module& {
  auto trace = ir::TraceRays{
    .width = self.lower_count(width),
    .height = self.lower_count(height),
    .depth = self.lower_count(depth),
  };
  return self.chain(PassKind::Compute, "trace rays", std::move(trace));
}

auto Module::end_pass(this Module& self, PassKind kind) -> std::vector<ValueID> {
  OX_ASSERT(self.ongoing_pass.has_value(), "no pass is open to close");
  OX_ASSERT(self.ongoing_pass->kind == kind, "the open pass is closed with the wrong kind of end");

  auto open = std::move(*self.ongoing_pass);
  self.ongoing_pass.reset();
  const auto completion = kind == PassKind::Rendering ? self.emit(ir::EndRendering{.pass = open.tip})
                                                      : self.emit(ir::EndCompute{.pass = open.tip});
  if (open.resources.empty()) {
    self.keep(completion);
  }
  auto results = std::vector<ValueID>{};
  for (const auto resource : open.resources) {
    results.push_back(self.emit(ir::PassResult{.completion = completion, .resource = resource}));
  }

  return results;
}

auto Module::end_rendering_values(this Module& self) -> std::vector<ValueID> {
  return self.end_pass(PassKind::Rendering);
}

auto Module::end_compute_values(this Module& self) -> std::vector<ValueID> { return self.end_pass(PassKind::Compute); }
} // namespace ox::vir
