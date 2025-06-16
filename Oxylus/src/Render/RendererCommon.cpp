#include "RendererCommon.hpp"

#include <vuk/RenderGraph.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "MeshVertex.hpp"
#include "Utils/OxMath.hpp"
#include "Utils/VukCommon.hpp"
#include "Vulkan/VkContext.hpp"

namespace ox {
vuk::Value<vuk::ImageAttachment>
RendererCommon::generate_cubemap_from_equirectangular(vuk::Value<vuk::ImageAttachment> hdr_image) {
  auto& allocator = App::get_vkcontext().superframe_allocator;
  if (!allocator->get_context().is_pipeline_available("equirectangular_to_cubemap")) {
    vuk::PipelineBaseCreateInfo equirectangular_to_cubemap;
    // equirectangular_to_cubemap.add_glsl(fs::read_shader_file("Cubemap.vert"), "Cubemap.vert");
    // equirectangular_to_cubemap.add_glsl(fs::read_shader_file("EquirectangularToCubemap.frag"),
    // "EquirectangularToCubemap.frag");
    allocator->get_context().create_named_pipeline("equirectangular_to_cubemap", equirectangular_to_cubemap);
  }

  constexpr auto size = 2048;
  auto attch = vuk::ImageAttachment::from_preset(
      vuk::ImageAttachment::Preset::eRTTCube, vuk::Format::eR32G32B32A32Sfloat, {size, size, 1}, vuk::Samples::e1);
  auto target = vuk::clear_image(vuk::declare_ia("cubemap", attch), vuk::Black<float>);

  const auto capture_projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
  const glm::mat4 capture_views[] = {
      lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
      lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
      lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
      lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
      lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
      lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f))};

  auto cubemap_pass = vuk::make_pass("env_cubemap",
                                     [capture_projection, capture_views](vuk::CommandBuffer& command_buffer,
                                                                         VUK_IA(vuk::eColorWrite) cube_map,
                                                                         VUK_IA(vuk::eFragmentSampled) hdr) {
    command_buffer.set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .broadcast_color_blend(vuk::BlendPreset::eOff)
        .set_rasterization({})
        .bind_image(0, 2, hdr)
        .bind_sampler(0, 2, vuk::LinearSamplerClamped)
        .bind_graphics_pipeline("equirectangular_to_cubemap");

    auto* projection = command_buffer.scratch_buffer<glm::mat4>(0, 0);
    *projection = capture_projection;
    const auto view = command_buffer.scratch_buffer<glm::mat4[6]>(0, 1);
    memcpy(view, capture_views, sizeof(capture_views));

    // const auto cube = generate_cube();
    // cube->bind_vertex_buffer(command_buffer);
    // cube->bind_index_buffer(command_buffer);
    // command_buffer.draw_indexed(cube->index_count, 6, 0, 0, 0);

    return cube_map;
  });

  auto envmap_output = cubemap_pass(hdr_image.mip(0), target);

  auto converge = vuk::make_pass(
      "converge", [](vuk::CommandBuffer& command_buffer, VUK_IA(vuk::eComputeRW) output) { return output; });
  return vuk::generate_mips(converge(envmap_output), target->level_count);
}

// std::shared_ptr<Mesh> RendererCommon::generate_quad() {
// if (mesh_lib.quad)
// return mesh_lib.quad;

// std::vector<Vertex> vertices(4);
// vertices[0].position = glm::vec3{-1.0f, -1.0f, 0.0f};
// vertices[0].uv = {};

// vertices[1].position = glm::vec3{1.0f, -1.0f, 0.0f};
// vertices[1].uv = glm::vec2{1.0f, 0.0f};

// vertices[2].position = glm::vec3{1.0f, 1.0f, 0.0f};
// vertices[2].uv = glm::vec2{1.0f, 1.0f};

// vertices[3].position = glm::vec3{-1.0f, 1.0f, 0.0f};
// vertices[3].uv = {0.0f, 1.0f};

// const auto indices = std::vector<uint32_t>{0, 1, 2, 2, 3, 0};

// mesh_lib.quad = std::make_shared<Mesh>(vertices, indices);

// return mesh_lib.quad;
// }

// std::shared_ptr<Mesh> RendererCommon::generate_cube() {
// if (mesh_lib.cube)
// return mesh_lib.cube;

// std::vector<Vertex> vertices(24);

// vertices[0].position = glm::vec3(0.5f, 0.5f, 0.5f);
// vertices[0].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, 1.0f)));

// vertices[1].position = glm::vec3(-0.5f, 0.5f, 0.5f);
// vertices[1].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, 1.0f)));

// vertices[2].position = glm::vec3(-0.5f, -0.5f, 0.5f);
// vertices[2].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, 1.0f)));

// vertices[3].position = glm::vec3(0.5f, -0.5f, 0.5f);
// vertices[3].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, 1.0f)));

// vertices[4].position = glm::vec3(0.5f, 0.5f, 0.5f);
// vertices[4].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(1.0f, 0.0f, 0.0f)));

// vertices[5].position = glm::vec3(0.5f, -0.5f, 0.5f);
// vertices[5].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(1.0f, 0.0f, 0.0f)));

// vertices[6].position = glm::vec3(0.5f, -0.5f, -0.5f);
// vertices[6].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(1.0f, 0.0f, 0.0f)));

// vertices[7].position = glm::vec3(0.5f, 0.5f, -0.5f);
// vertices[7].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(1.0f, 0.0f, 0.0f)));

// vertices[8].position = glm::vec3(0.5f, 0.5f, 0.5f);
// vertices[8].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 1.0f, 0.0f)));

// vertices[9].position = glm::vec3(0.5f, 0.5f, -0.5f);
// vertices[9].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 1.0f, 0.0f)));

// vertices[10].position = glm::vec3(-0.5f, 0.5f, -0.5f);
// vertices[10].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 1.0f, 0.0f)));

// vertices[11].position = glm::vec3(-0.5f, 0.5f, 0.5f);
// vertices[11].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 1.0f, 0.0f)));

// vertices[12].position = glm::vec3(-0.5f, 0.5f, 0.5f);
// vertices[12].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(-1.0f, 0.0f, 0.0f)));

// vertices[13].position = glm::vec3(-0.5f, 0.5f, -0.5f);
// vertices[13].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(-1.0f, 0.0f, 0.0f)));

// vertices[14].position = glm::vec3(-0.5f, -0.5f, -0.5f);
// vertices[14].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(-1.0f, 0.0f, 0.0f)));

// vertices[15].position = glm::vec3(-0.5f, -0.5f, 0.5f);
// vertices[15].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(-1.0f, 0.0f, 0.0f)));

// vertices[16].position = glm::vec3(-0.5f, -0.5f, -0.5f);
// vertices[16].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, -1.0f, 0.0f)));

// vertices[17].position = glm::vec3(0.5f, -0.5f, -0.5f);
// vertices[17].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, -1.0f, 0.0f)));

// vertices[18].position = glm::vec3(0.5f, -0.5f, 0.5f);
// vertices[18].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, -1.0f, 0.0f)));

// vertices[19].position = glm::vec3(-0.5f, -0.5f, 0.5f);
// vertices[19].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, -1.0f, 0.0f)));

// vertices[20].position = glm::vec3(0.5f, -0.5f, -0.5f);
// vertices[20].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, -1.0f)));

// vertices[21].position = glm::vec3(-0.5f, -0.5f, -0.5f);
// vertices[21].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, -1.0f)));

// vertices[22].position = glm::vec3(-0.5f, 0.5f, -0.5f);
// vertices[22].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, -1.0f)));

// vertices[23].position = glm::vec3(0.5f, 0.5f, -0.5f);
// vertices[23].normal = glm::packSnorm2x16(math::float32x3_to_oct(glm::vec3(0.0f, 0.0f, -1.0f)));

// for (int i = 0; i < 6; i++) {
// vertices[i * 4 + 0].uv = glm::vec2(0.0f, 0.0f);
// vertices[i * 4 + 1].uv = glm::vec2(1.0f, 0.0f);
// vertices[i * 4 + 2].uv = glm::vec2(1.0f, 1.0f);
// vertices[i * 4 + 3].uv = glm::vec2(0.0f, 1.0f);
// }

// std::vector<uint32_t> indices = {0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
// 12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};

// mesh_lib.cube = std::make_shared<Mesh>(vertices, indices);

// return mesh_lib.cube;
// }

// std::shared_ptr<Mesh> RendererCommon::generate_sphere() {
// if (mesh_lib.sphere)
// return mesh_lib.sphere;

// std::vector<Vertex> vertices;
// std::vector<uint32_t> indices;

// int latitude_bands = 30;
// int longitude_bands = 30;
// float radius = 2;

// for (int i = 0; i <= latitude_bands; i++) {
// float theta = (float)i * glm::pi<float>() / (float)latitude_bands;
// float sinTheta = sin(theta);
// float cosTheta = cos(theta);

// for (int longNumber = 0; longNumber <= longitude_bands; longNumber++) {
// float phi = (float)longNumber * 2.0f * glm::pi<float>() / (float)longitude_bands;
// float sinPhi = sin(phi);
// float cosPhi = cos(phi);

// Vertex vs;
// glm::vec3 normal = {};
// normal[0] = cosPhi * sinTheta;                                // x
// normal[1] = cosTheta;                                         // y
// normal[2] = sinPhi * sinTheta;                                // z
// vs.uv[0] = 1.0f - (float)longNumber / (float)longitude_bands; // u
// vs.uv[1] = 1.0f - (float)i / (float)latitude_bands;           // v
// vs.position[0] = radius * normal[0];
// vs.position[1] = radius * normal[1];
// vs.position[2] = radius * normal[2];
// vs.normal = glm::packSnorm2x16(math::float32x3_to_oct(normal));

// vertices.push_back(vs);
// }

// for (int j = 0; j < latitude_bands; j++) {
// for (int longNumber = 0; longNumber < longitude_bands; longNumber++) {
// int first = j * (longitude_bands + 1) + longNumber;
// int second = first + longitude_bands + 1;

// indices.push_back(first);
// indices.push_back(second);
// indices.push_back(first + 1);

// indices.push_back(second);
// indices.push_back(second + 1);
// indices.push_back(first + 1);
// }
// }
// }

// mesh_lib.sphere = std::make_shared<Mesh>(vertices, indices);

// return mesh_lib.sphere;
// }

vuk::Value<vuk::ImageAttachment> RendererCommon::apply_blur(const vuk::Value<vuk::ImageAttachment>& src_attachment,
                                                            const vuk::Value<vuk::ImageAttachment>& dst_attachment) {
  auto& allocator = *App::get_vkcontext().superframe_allocator;
  if (!allocator.get_context().is_pipeline_available("gaussian_blur_pipeline")) {
    vuk::PipelineBaseCreateInfo pci;
    // pci.add_hlsl(fs::read_shader_file("FullscreenTriangle.hlsl"), fs::get_shader_path("FullscreenTriangle.hlsl"),
    // vuk::HlslShaderStage::eVertex); pci.add_glsl(fs::read_shader_file("PostProcess/SinglePassGaussianBlur.frag"),
    // "PostProcess/SinglePassGaussianBlur.frag");
    allocator.get_context().create_named_pipeline("gaussian_blur_pipeline", pci);
  }

  auto pass = vuk::make_pass(
      "blur", [](vuk::CommandBuffer& command_buffer, VUK_IA(vuk::eFragmentSampled) src, VUK_IA(vuk::eColorRW) target) {
    command_buffer.bind_graphics_pipeline("gaussian_blur_pipeline")
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .broadcast_color_blend(vuk::BlendPreset::eOff)
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .bind_image(0, 0, src)
        .bind_sampler(0, 0, vuk::LinearSamplerClamped)
        .draw(3, 1, 0, 0);

    return target;
  });

  return pass(src_attachment, dst_attachment);
}
} // namespace ox
