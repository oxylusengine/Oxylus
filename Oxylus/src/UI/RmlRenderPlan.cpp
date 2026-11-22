#include "UI/RmlRenderPlan.hpp"

#include <algorithm>

namespace ox {
static auto pack_pass(const RmlPass& pass) -> u32 {
  return static_cast<u32>(pass.kind) | (static_cast<u32>(pass.blend_mode) << 4) |
         (static_cast<u32>(pass.clip_write) << 6) | (static_cast<u32>(pass.stencil) << 8);
}

auto RmlFilter::is_blurred(this const RmlFilter& self) -> bool {
  return self.kind == RmlFilterKind::Blur || (self.kind == RmlFilterKind::DropShadow && self.value >= 0.5f);
}

auto RmlRenderPlan::signature(this const RmlRenderPlan& self, std::vector<u32>& out) -> void {
  out.clear();
  out.push_back(static_cast<u32>(self.extent.x));
  out.push_back(static_cast<u32>(self.extent.y));
  out.push_back(self.layer_count | (static_cast<u32>(self.mask_count) << 16));
  out.push_back(static_cast<u32>(self.saved_textures.size()) | (self.uses_stencil ? 1_u32 << 31 : 0_u32));

  for (const auto& pass : self.passes) {
    out.push_back(pack_pass(pass));
    out.push_back(static_cast<u32>(pass.layer) | (static_cast<u32>(pass.source) << 16));

    switch (pass.kind) {
      case RmlPassKind::Draw: {
        out.push_back(pass.read_count);
        for (u32 i = 0; i < pass.read_count; i++) {
          out.push_back(static_cast<u32>(self.texture_reads[pass.first_read + i]));
        }
      } break;
      case RmlPassKind::Composite: {
        out.push_back(pass.filter_count);
        for (u32 i = 0; i < pass.filter_count; i++) {
          const auto& filter = self.filters[pass.first_filter + i];
          out.push_back(
            static_cast<u32>(filter.kind) | (filter.is_blurred() ? 1_u32 << 8 : 0_u32) |
            (static_cast<u32>(filter.mask) << 16)
          );
        }
      } break;
      case RmlPassKind::SaveTexture: out.push_back(static_cast<u32>(pass.saved_texture)); break;
      case RmlPassKind::SaveMask   : out.push_back(static_cast<u32>(pass.mask)); break;
      default                      : break;
    }
  }
}

auto RmlPlanBuilder::begin(this RmlPlanBuilder& self, glm::ivec2 extent) -> void {
  self.plan = {.extent = extent};
  self.layer_stack.clear();
  self.layer_stack.push_back(RmlLayerID::Base);
  self.scissor_region = {};
  self.is_scissor_enabled = false;
  self.is_clip_enabled = false;
  self.clip_reference = 0;
}

auto RmlPlanBuilder::set_scissor_enabled(this RmlPlanBuilder& self, bool is_enabled) -> void {
  self.is_scissor_enabled = is_enabled;
}

auto RmlPlanBuilder::set_scissor(this RmlPlanBuilder& self, glm::ivec4 region) -> void { self.scissor_region = region; }

auto RmlPlanBuilder::scissor(this const RmlPlanBuilder& self) -> glm::ivec4 {
  const auto extent = self.plan.extent;
  if (!self.is_scissor_enabled) {
    return {0, 0, extent.x, extent.y};
  }

  const auto& region = self.scissor_region;
  const auto x0 = std::clamp(region.x, 0, extent.x);
  const auto y0 = std::clamp(region.y, 0, extent.y);
  const auto x1 = std::clamp(region.x + region.z, x0, extent.x);
  const auto y1 = std::clamp(region.y + region.w, y0, extent.y);

  return {x0, y0, x1 - x0, y1 - y0};
}

auto RmlPlanBuilder::stencil_test(this const RmlPlanBuilder& self) -> u8 {
  return self.is_clip_enabled ? self.clip_reference : 0_u8;
}

auto RmlPlanBuilder::draw(this RmlPlanBuilder& self, RmlDrawCmd cmd) -> void {
  auto& plan = self.plan;
  cmd.scissor = self.scissor();

  const auto layer = self.top_layer();
  const auto stencil = self.stencil_test();
  const auto is_continued = !plan.passes.empty() && plan.passes.back().kind == RmlPassKind::Draw &&
                            plan.passes.back().layer == layer && plan.passes.back().stencil == stencil;
  if (!is_continued) {
    plan.passes.push_back({
      .kind = RmlPassKind::Draw,
      .layer = layer,
      .stencil = stencil,
      .first_draw = static_cast<u32>(plan.draws.size()),
      .first_read = static_cast<u32>(plan.texture_reads.size()),
    });
  }

  auto& pass = plan.passes.back();
  if (cmd.texture) {
    const auto saved = std::ranges::find(plan.saved_textures, cmd.texture);
    if (saved != plan.saved_textures.end()) {
      const auto slot = static_cast<RmlSavedTextureID>(saved - plan.saved_textures.begin());
      const auto reads = std::span(plan.texture_reads).subspan(pass.first_read, pass.read_count);
      if (std::ranges::find(reads, slot) == reads.end()) {
        plan.texture_reads.push_back(slot);
        pass.read_count += 1;
      }
    }
  }

  plan.draws.push_back(cmd);
  pass.draw_count += 1;
}

auto RmlPlanBuilder::set_clip_mask_enabled(this RmlPlanBuilder& self, bool is_enabled) -> void {
  self.is_clip_enabled = is_enabled;
}

auto RmlPlanBuilder::clip(this RmlPlanBuilder& self, Rml::ClipMaskOperation operation, RmlDrawCmd cmd) -> void {
  auto& plan = self.plan;
  cmd.scissor = self.scissor();
  plan.uses_stencil = true;

  auto pass = RmlPass{
    .kind = RmlPassKind::ClipDraw,
    .first_draw = static_cast<u32>(plan.draws.size()),
    .draw_count = 1,
  };
  switch (operation) {
    case Rml::ClipMaskOperation::Set: {
      plan.passes.push_back({.kind = RmlPassKind::ClipClear, .stencil = 0});
      pass.clip_write = RmlClipWrite::Replace;
      pass.stencil = 1;
      self.clip_reference = 1;
    } break;
    case Rml::ClipMaskOperation::SetInverse: {
      plan.passes.push_back({.kind = RmlPassKind::ClipClear, .stencil = 1});
      pass.clip_write = RmlClipWrite::Replace;
      pass.stencil = 0;
      self.clip_reference = 1;
    } break;
    case Rml::ClipMaskOperation::Intersect: {
      pass.clip_write = RmlClipWrite::Increment;
      pass.stencil = self.clip_reference;
      self.clip_reference = static_cast<u8>(std::min(self.clip_reference + 1, 255));
    } break;
  }

  plan.passes.push_back(pass);
  plan.draws.push_back(cmd);
}

auto RmlPlanBuilder::push_layer(this RmlPlanBuilder& self) -> RmlLayerID {
  auto& plan = self.plan;
  const auto layer = static_cast<RmlLayerID>(plan.layer_count);
  plan.layer_count += 1;
  self.layer_stack.push_back(layer);
  plan.passes.push_back({.kind = RmlPassKind::PushLayer, .layer = layer});

  return layer;
}

auto RmlPlanBuilder::composite(
  this RmlPlanBuilder& self,
  RmlLayerID source,
  RmlLayerID destination,
  Rml::BlendMode blend_mode,
  std::span<const RmlFilter> filters
) -> void {
  auto& plan = self.plan;
  const auto layer_count = static_cast<u16>(plan.layer_count);
  if (static_cast<u16>(source) >= layer_count || static_cast<u16>(destination) >= layer_count) {
    return;
  }

  plan.passes.push_back({
    .kind = RmlPassKind::Composite,
    .layer = destination,
    .source = source,
    .blend_mode = blend_mode,
    .stencil = self.stencil_test(),
    .first_filter = static_cast<u32>(plan.filters.size()),
    .filter_count = static_cast<u32>(filters.size()),
    .scissor = self.scissor(),
  });
  plan.filters.insert(plan.filters.end(), filters.begin(), filters.end());
}

auto RmlPlanBuilder::pop_layer(this RmlPlanBuilder& self) -> void {
  if (self.layer_stack.size() > 1) {
    self.layer_stack.pop_back();
  }
}

auto RmlPlanBuilder::top_layer(this const RmlPlanBuilder& self) -> RmlLayerID { return self.layer_stack.back(); }

auto RmlPlanBuilder::save_texture(this RmlPlanBuilder& self, Rml::TextureHandle texture) -> RmlSavedTextureID {
  auto& plan = self.plan;
  const auto slot = static_cast<RmlSavedTextureID>(plan.saved_textures.size());
  plan.saved_textures.push_back(texture);
  plan.passes.push_back({
    .kind = RmlPassKind::SaveTexture,
    .layer = self.top_layer(),
    .saved_texture = slot,
    .scissor = self.scissor(),
  });

  return slot;
}

auto RmlPlanBuilder::save_mask(this RmlPlanBuilder& self) -> RmlMaskID {
  auto& plan = self.plan;
  const auto mask = static_cast<RmlMaskID>(plan.mask_count);
  plan.mask_count += 1;
  plan.passes.push_back({.kind = RmlPassKind::SaveMask, .layer = self.top_layer(), .mask = mask});

  return mask;
}
} // namespace ox
