#include "UI/RmlRenderer.hpp"

#include "vuk/runtime/CommandBuffer.hpp"

namespace ox {
auto RmlRenderer::begin_frame(this RmlRenderer& self) -> void {
  self.frame_indices.clear();
  self.frame_vertices.clear();
  self.draw_commands.clear();
  self.current_scissor_enabled = false;
}

auto RmlRenderer::end_frame(this RmlRenderer& self, RenderContext& context, vuk::Value<vuk::ImageAttachment> target)
  -> vuk::Value<vuk::ImageAttachment> {
  usize vertex_size = self.frame_vertices.size() * sizeof(Rml::Vertex);
  usize index_size = self.frame_indices.size() * sizeof(i32);

  if (self.draw_commands.empty() || vertex_size <= 0 || index_size <= 0) {
    return target;
  }

  auto vertex_buffer = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, vertex_size, 1);
  auto index_buffer = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, index_size, 1);

  std::memcpy(vertex_buffer->mapped_ptr, self.frame_vertices.data(), vertex_size);
  std::memcpy(index_buffer->mapped_ptr, self.frame_indices.data(), index_size);

  std::unordered_map<RmlTextureID, uint32_t> acquired_texture_cache = {};
  std::vector<vuk::Value<vuk::ImageAttachment>> frame_textures = {};
  frame_textures.emplace_back(self.white_texture->acquire());
  for (auto& cmd : self.draw_commands) {
    if (!cmd.texture) {
      cmd.texture_array_index = 0;
      continue;
    }

    auto tex_id = static_cast<RmlTextureID>(cmd.texture);

    auto it = acquired_texture_cache.find(tex_id);
    if (it != acquired_texture_cache.end()) {
      cmd.texture_array_index = it->second;
    } else {
      if (auto* texture_ptr = self.loaded_textures.slot(tex_id)) {
        Texture* actual_texture = texture_ptr->get();

        uint32_t new_index = static_cast<uint32_t>(frame_textures.size());
        frame_textures.push_back(actual_texture->acquire());

        acquired_texture_cache[tex_id] = new_index;
        cmd.texture_array_index = new_index;
      } else {
        cmd.texture_array_index = 0;
      }
    }
  }

  auto textures_array = vuk::declare_array("rml_sampled_textures", std::span(frame_textures));

  return vuk::make_pass("rmlui", //
    [dc = self.draw_commands](vuk::CommandBuffer& command_buffer,
      VUK_BA(vuk::Access::eVertexRead) vertex_buf,
      VUK_BA(vuk::Access::eIndexRead) index_buf,
      VUK_IA(vuk::eColorWrite) color_rt,
      VUK_ARG(vuk::ImageAttachment[], vuk::Access::eFragmentSampled) bound_textures) {
        // Premultiplied alpha
        vuk::PipelineColorBlendAttachmentState blend_state = {
          .blendEnable = true,
          .srcColorBlendFactor = vuk::BlendFactor::eOne,
          .dstColorBlendFactor = vuk::BlendFactor::eOneMinusSrcAlpha,
          .colorBlendOp = vuk::BlendOp::eAdd,
          .srcAlphaBlendFactor = vuk::BlendFactor::eOne,
          .dstAlphaBlendFactor = vuk::BlendFactor::eOneMinusSrcAlpha,
          .alphaBlendOp = vuk::BlendOp::eAdd
        };

    command_buffer.set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_rasterization(vuk::PipelineRasterizationStateCreateInfo{})
        .set_color_blend(color_rt, blend_state)
        .bind_graphics_pipeline("rmlui")
        .bind_vertex_buffer(
          0,
          vertex_buf,
          0,
          vuk::Packed{
            vuk::Format::eR32G32Sfloat,   // Position
            vuk::Format::eR8G8B8A8Unorm,  // Color
            vuk::Format::eR32G32Sfloat    // TexCoord
          }
        )
        .bind_index_buffer(index_buf, vuk::IndexType::eUint32)
        .set_viewport(0, vuk::Rect2D::framebuffer());

      for (const auto& cmd : dc) {
        if (cmd.scissor_enabled) {
          vuk::Rect2D scissor;
          scissor.offset = {cmd.scissor.x, cmd.scissor.y};
          scissor.extent = {static_cast<u32>(cmd.scissor.z), static_cast<u32>(cmd.scissor.w)};
          command_buffer.set_scissor(0, scissor);
        } else {
          command_buffer.set_scissor(0, vuk::Rect2D::framebuffer());
        }

        struct PushConstant {
          f32 translation[2] = {};
          f32 screen_size[2] = {};
          f32 transform[16] = {};
        } pc;
        pc.translation[0] = cmd.translation.x;
        pc.translation[1] = cmd.translation.y;
        pc.screen_size[0] = static_cast<f32>(color_rt->extent.width);
        pc.screen_size[1] = static_cast<f32>(color_rt->extent.height);
        std::memcpy(pc.transform, cmd.transform.data(), sizeof(float) * 16);
        command_buffer.bind_image(0, 1, bound_textures[cmd.texture_array_index])
                      .bind_sampler(0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear})
                      .push_constants(vuk::ShaderStageFlagBits::eVertex, 0, pc)
                      .draw_indexed(cmd.index_count, 1, cmd.index_offset, cmd.vertex_offset, 0);
      }

      return color_rt;
    })(std::move(vertex_buffer), std::move(index_buffer), std::move(target), std::move(textures_array));
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

auto RmlRenderer::set_white_texture(this RmlRenderer& self, Texture* texture) -> void {
  ZoneScoped;

  self.white_texture = texture;
}

auto RmlRenderer::RenderGeometry(
  Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation, Rml::TextureHandle texture_handle
) -> void {
  auto id = static_cast<RmlGeometryID>(geometry_handle);

  if (auto* geo = this->compiled_geometries.slot(id)) {
    this->render_geometry(
      geo->vertices.data(),
      geo->vertices.size(),
      geo->indices.data(),
      geo->indices.size(),
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

  auto texture = std::make_unique<Texture>();
  texture->create(source, {});

  texture_dimensions.x = texture->get_extent().width;
  texture_dimensions.y = texture->get_extent().height;

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

  auto texture = std::make_unique<Texture>();
  texture->create(
    {},
    TextureLoadInfo{
      .format = vuk::Format::eR8G8B8A8Unorm,
      .loaded_data = straight_alpha_data.data(),
      .extent = vuk::Extent3D{static_cast<u32>(source_dimensions.x), static_cast<u32>(source_dimensions.y), 1u}
    }
  );

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
