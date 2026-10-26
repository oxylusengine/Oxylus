#include "UI/RmlRenderer.hpp"

#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/SlotMap.hpp"
#include "Render/RenderContext.hpp"
#include "UI/RmlFileInterface.hpp"
#include "Utils/Log.hpp"

namespace ox {
RmlRenderer::RmlRenderer() {
  ZoneScoped;

  const u8 white_pixel[] = {0xFF, 0xFF, 0xFF, 0xFF};
  this->white_texture = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = {1, 1, 1},
    .usage = vir::ImageUsageFlags::Sampled,
  });
  this->white_texture.upload(white_pixel, vir::Access::FragmentSampled);
}

RmlRenderer::~RmlRenderer() = default;

auto RmlRenderer::begin_frame(this RmlRenderer& self) -> void {
  self.frame_indices.clear();
  self.frame_vertices.clear();
  self.draw_commands.clear();
  self.current_scissor_enabled = false;
}

auto RmlRenderer::upload(this RmlRenderer& self, RenderContext& context) -> void {
  ZoneScoped;

  self.uploaded_commands.clear();
  if (self.draw_commands.empty() || self.frame_vertices.empty() || self.frame_indices.empty()) {
    return;
  }

  auto vertices = context.upload_frame_buffer(
    std::span(
      reinterpret_cast<const u8*>(self.frame_vertices.data()),
      self.frame_vertices.size() * sizeof(Rml::Vertex)
    ),
    "rmlui vertices"
  );
  auto indices = context.upload_frame_buffer(
    std::span(reinterpret_cast<const u8*>(self.frame_indices.data()), self.frame_indices.size() * sizeof(i32)),
    "rmlui indices"
  );
  if (!vertices || !indices) {
    OX_LOG_ERROR("failed to stage the rmlui geometry");
    return;
  }

  self.vertex_buffer = *vertices;
  self.index_buffer = *indices;
  self.uploaded_commands = std::move(self.draw_commands);
  self.draw_commands.clear();
  self.frame_vertices.clear();
  self.frame_indices.clear();
}

auto RmlRenderer::record(
  this RmlRenderer& self, vir::Module& module, RenderContext& render_context, vir::ValueID target
) -> vir::ValueID {
  ZoneScoped;

  // premultiplied alpha
  static constexpr auto blend_state = vir::ColorBlendAttachmentState{
    .is_blend_enabled = true,
    .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
    .dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .color_blend_op = VK_BLEND_OP_ADD,
    .src_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
    .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alpha_blend_op = VK_BLEND_OP_ADD,
  };
  static constexpr auto sampler_info = vir::SamplerInfo{
    .mag_filter = VK_FILTER_LINEAR,
    .min_filter = VK_FILTER_LINEAR,
    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  };
  const auto sampler_index = SlotMap_decode_id(render_context.cached_sampler(sampler_info)).index;

  const auto draws = module.declare_callback_var("rmlui", [&self, sampler_index](vir::Recorder& recorder) -> void {
    if (self.uploaded_commands.empty()) {
      return;
    }

    const auto area = recorder.render_area();
    recorder.bind_index_buffer(self.index_buffer, VK_INDEX_TYPE_UINT32);
    for (const auto& cmd : self.uploaded_commands) {
      if (cmd.scissor_enabled) {
        recorder.set_scissor_absolute({
          .offset = {cmd.scissor.x, cmd.scissor.y},
          .extent = {static_cast<u32>(cmd.scissor.z), static_cast<u32>(cmd.scissor.w)},
        });
      } else {
        recorder.set_scissor_absolute(area);
      }

      const auto* texture = cmd.texture ? self.loaded_textures.slot(static_cast<RmlTextureID>(cmd.texture)) : nullptr;
      struct PushConstant {
        f32 transform[16] = {};
        u64 vertices = 0;
        f32 translation[2] = {};
        f32 screen_size[2] = {};
        u32 image_index = 0;
        u32 sampler_index = 0;
      } push = {
        .vertices = self.vertex_buffer.device_address + cmd.vertex_offset * sizeof(Rml::Vertex),
        .translation = {cmd.translation.x, cmd.translation.y},
        .screen_size = {static_cast<f32>(area.extent.width), static_cast<f32>(area.extent.height)},
        .image_index = (texture ? *texture : self.white_texture).get_view_index(),
        .sampler_index = sampler_index,
      };
      std::memcpy(push.transform, cmd.transform.data(), sizeof(push.transform));
      recorder.push_constants(push).draw_indexed(cmd.index_count, 1, cmd.index_offset, 0, 0);
    }
  });

  return module.begin_rendering({{target, vir::Access::ColorRW}})
    .with_name("rmlui")
    .bind_graphics_pipeline(render_context.pipeline("rmlui"))
    .set_color_blend(0, blend_state)
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .record_from(draws)
    .end_rendering()[0];
}

auto RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
  -> Rml::CompiledGeometryHandle {
  ZoneScoped;

  RmlCompiledGeometry geo = {};
  geo.vertices.assign(vertices.begin(), vertices.end());
  geo.indices.assign(indices.begin(), indices.end());

  auto id = this->compiled_geometries.create_slot(std::move(geo));
  return static_cast<Rml::CompiledGeometryHandle>(id);
}

auto RmlRenderer::render_geometry(
  this RmlRenderer& self,
  Rml::Vertex* vertices,
  int num_vertices,
  int* indices,
  int num_indices,
  Rml::TextureHandle texture,
  const Rml::Vector2f& translation
) -> void {
  ZoneScoped;

  RmlDrawCmd draw_cmd = {
    .index_count = static_cast<u32>(num_indices),
    .index_offset = static_cast<u32>(self.frame_indices.size()),
    .vertex_offset = static_cast<u32>(self.frame_vertices.size()),
    .texture = texture,
    .translation = translation,
    .transform = self.current_transform,
    .scissor_enabled = self.current_scissor_enabled,
    .scissor = self.current_scissor,
  };

  self.frame_vertices.insert(self.frame_vertices.end(), vertices, vertices + num_vertices);
  self.frame_indices.insert(self.frame_indices.end(), indices, indices + num_indices);

  self.draw_commands.push_back(draw_cmd);
}

auto RmlRenderer::RenderGeometry(
  Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation, Rml::TextureHandle texture_handle
) -> void {
  auto id = static_cast<RmlGeometryID>(geometry_handle);

  if (auto* geo = this->compiled_geometries.slot(id)) {
    this->render_geometry(
      geo->vertices.data(),
      static_cast<int>(geo->vertices.size()),
      geo->indices.data(),
      static_cast<int>(geo->indices.size()),
      texture_handle,
      translation
    );
  }
}
auto RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) -> void {
  ZoneScoped;

  auto id = static_cast<RmlGeometryID>(geometry);
  this->compiled_geometries.destroy_slot(id);
}

auto RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) -> Rml::TextureHandle {
  ZoneScoped;

  // joined against the document's own path by RmlUi, so as virtual as the document was. The source image while it is
  // there, its cooked pack once it isn't, as `RmlFileInterface` reads documents: a shipped game strips the image
  const auto& vfs = App::get_vfs();
  const auto path = resolve_rml_path(&vfs, source);
  auto texture = Texture{};
  if (std::filesystem::exists(path)) {
    texture = Texture::create({.source = path});
  } else if (App::has_mod<AssetManager>()) {
    const auto pack_path = find_rml_pack(App::mod<AssetManager>(), &vfs, source, {AssetType::Texture});
    if (auto pack = pack_path.empty() ? nullopt : AssetFile::unpack(pack_path)) {
      for (const auto& entry : pack->entries) {
        if (const auto* texture_data = std::get_if<TextureData>(&entry.data)) {
          texture = Texture::create(*texture_data, TextureLoadInfo{});
          break;
        }
      }
    }
  }

  if (!texture) {
    OX_LOG_WARN("RmlUi couldn't load the image '{}' ({}).", source, path);
    return 0;
  }

  texture_dimensions.x = texture.get_extent().width;
  texture_dimensions.y = texture.get_extent().height;

  auto id = this->loaded_textures.create_slot(std::move(texture));
  return static_cast<Rml::TextureHandle>(id);
}

auto RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
  -> Rml::TextureHandle {
  ZoneScoped;

  std::vector<uint8_t> straight_alpha_data(source.begin(), source.end());

  // Undo RmlUi's native premultiplication so the shader can universally handle straight alpha
  for (size_t i = 0; i < straight_alpha_data.size(); i += 4) {
    uint8_t alpha = straight_alpha_data[i + 3];
    // Only divide if the pixel is semi-transparent
    if (alpha > 0 && alpha < 255) {
      straight_alpha_data[i + 0] = static_cast<uint8_t>(std::min(255, (straight_alpha_data[i + 0] * 255) / alpha));
      straight_alpha_data[i + 1] = static_cast<uint8_t>(std::min(255, (straight_alpha_data[i + 1] * 255) / alpha));
      straight_alpha_data[i + 2] = static_cast<uint8_t>(std::min(255, (straight_alpha_data[i + 2] * 255) / alpha));
    }
  }

  auto texture = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = {static_cast<u32>(source_dimensions.x), static_cast<u32>(source_dimensions.y), 1_u32},
    .usage = vir::ImageUsageFlags::Sampled,
  });
  texture.upload(straight_alpha_data, vir::Access::FragmentSampled);

  auto id = this->loaded_textures.create_slot(std::move(texture));
  return static_cast<Rml::TextureHandle>(id);
}

auto RmlRenderer::ReleaseTexture(Rml::TextureHandle texture_handle) -> void {
  ZoneScoped;

  auto id = static_cast<RmlTextureID>(texture_handle);
  this->loaded_textures.destroy_slot(id);
}

auto RmlRenderer::EnableScissorRegion(bool enable) -> void {
  ZoneScoped;

  this->current_scissor_enabled = enable;
}

auto RmlRenderer::SetScissorRegion(Rml::Rectanglei region) -> void {
  ZoneScoped;

  this->current_scissor.x = region.TopLeft().x;
  this->current_scissor.y = region.TopLeft().y;
  this->current_scissor.z = region.Width();
  this->current_scissor.w = region.Height();
}

auto RmlRenderer::SetTransform(const Rml::Matrix4f* transform) -> void {
  ZoneScoped;

  if (transform) {
    this->current_transform = *transform;
  } else {
    this->current_transform = Rml::Matrix4f::Identity();
  }
}
} // namespace ox
