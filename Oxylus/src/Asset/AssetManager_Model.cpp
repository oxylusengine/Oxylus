#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <ranges>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Render/AccelerationStructure.hpp"
#include "Render/UploadBatch.hpp"

namespace ox {
struct UploadedMesh {
  GPU::Mesh gpu_mesh = {};
  std::array<GPU::MeshLOD, GPU::Mesh::MAX_LODS> lods = {};
  vuk::Unique<vuk::Buffer> buffer;
  AccelerationStructure blas = {};
};

auto upload_mesh(RenderContext& render_context, const ModelData::Mesh& mesh, UploadBatch* batch) -> UploadedMesh {
  ZoneScoped;

  auto result = UploadedMesh();
  if (mesh.lod_count == 0 || mesh.blob.empty()) {
    return result;
  }

  auto gpu_buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, mesh.blob.size());
  const auto gpu_mesh_bda = gpu_buffer->device_address;

  auto& gpu_mesh = result.gpu_mesh;
  gpu_mesh.vertex_count = mesh.vertex_count;
  gpu_mesh.lod_count = mesh.lod_count;
  gpu_mesh.bounds.aabb_center = glm::make_vec3(mesh.bounds_center);
  gpu_mesh.bounds.aabb_extent = glm::make_vec3(mesh.bounds_extent);
  gpu_mesh.vertex_positions = gpu_mesh_bda + mesh.vertex_positions_offset;
  gpu_mesh.vertex_normals = gpu_mesh_bda + mesh.vertex_normals_offset;
  if (mesh.has_texture_coords) {
    gpu_mesh.texture_coords = gpu_mesh_bda + mesh.texture_coords_offset;
  }
  gpu_mesh.lods = gpu_mesh_bda + mesh.lod_metadata_offset;

  result.lods = mesh.lods;
  for (auto lod_index = 0_u32; lod_index < gpu_mesh.lod_count; lod_index++) {
    auto& lod = result.lods[lod_index];
    lod.indices += gpu_mesh_bda;
    lod.meshlets += gpu_mesh_bda;
    lod.meshlet_bounds += gpu_mesh_bda;
    lod.local_triangle_indices += gpu_mesh_bda;
    lod.indirect_vertex_indices += gpu_mesh_bda;
  }

  auto staging_buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eCPUonly, mesh.blob.size());
  std::memcpy(staging_buffer->mapped_ptr, mesh.blob.data(), mesh.blob.size());
  // the compiler reserves the LOD table at the blob tail but leaves it zeroed: every offset in it
  // only becomes an address once there is a buffer to add
  std::memcpy(
    reinterpret_cast<u8*>(staging_buffer->mapped_ptr) + mesh.lod_metadata_offset,
    result.lods.data(),
    gpu_mesh.lod_count * sizeof(GPU::MeshLOD)
  );

  auto gpu_mesh_value = vuk::discard_buf("mesh", *gpu_buffer);
  auto staging_value = vuk::acquire_buf("mesh staging", *staging_buffer, vuk::Access::eNone);
  auto upload = render_context.upload_staging(std::move(staging_value), std::move(gpu_mesh_value));

  auto blas_scratch = vuk::Unique<vuk::Buffer>();
  upload = build_mesh_blas(
    render_context,
    BLASBuildInfo{
      .vertex_positions = gpu_mesh.vertex_positions,
      .indices = result.lods[0].indices,
      .vertex_count = gpu_mesh.vertex_count,
      .index_count = result.lods[0].indices_count,
    },
    std::move(upload),
    result.blas,
    blas_scratch
  );

  if (batch) {
    auto values = std::array<vuk::UntypedValue, 1>{std::move(upload)};
    render_context.submit_multiple(values);
    batch->add_upload(values);
    auto owned = std::array<vuk::Unique<vuk::Buffer>, 2>{std::move(staging_buffer), std::move(blas_scratch)};
    batch->take_staging(owned);
  } else {
    render_context.wait_on(std::move(upload));
  }

  result.buffer = std::move(gpu_buffer);

  return result;
}

auto to_collision_mesh(const ModelData::Mesh& mesh) -> Model::CollisionMesh {
  ZoneScoped;

  auto collision = Model::CollisionMesh{};
  collision.positions.reserve(mesh.collision_positions.size() / 3);
  for (auto i = 0_sz; i + 2 < mesh.collision_positions.size(); i += 3) {
    collision.positions
      .emplace_back(mesh.collision_positions[i], mesh.collision_positions[i + 1], mesh.collision_positions[i + 2]);
  }
  collision.indices = mesh.collision_indices;

  return collision;
}

auto to_material(const ModelData::Material& src, std::span<const UUID> textures) -> Material {
  const auto resolve = [&](u32 index) -> UUID {
    return index < textures.size() ? textures[index] : UUID(nullptr);
  };

  return Material{
    .albedo_color = glm::make_vec4(src.albedo_color),
    .uv_size = glm::make_vec2(src.uv_size),
    .uv_offset = glm::make_vec2(src.uv_offset),
    .emissive_color = glm::make_vec3(src.emissive_color),
    .roughness_factor = src.roughness_factor,
    .metallic_factor = src.metallic_factor,
    .alpha_mode = src.alpha_mode,
    .alpha_cutoff = src.alpha_cutoff,
    .sampling_mode = src.sampling_mode,
    .flip_normal_y = src.flip_normal_y,
    .albedo_texture = resolve(src.albedo_texture_index),
    .normal_texture = resolve(src.normal_texture_index),
    .emissive_texture = resolve(src.emissive_texture_index),
    .metallic_roughness_texture = resolve(src.metallic_roughness_texture_index),
    .occlusion_texture = resolve(src.occlusion_texture_index),
  };
}

auto AssetManager::load_model(this AssetManager& self, ModelData&& model_data, bool async) -> ModelID {
  ZoneScoped;

  auto& job_man = App::get_job_manager();
  auto& render_context = App::get_rendercontext();

  // the mesh jobs outlive this call when `async`, so the source has to be owned by them
  auto data = std::make_shared<const ModelData>(std::move(model_data));

  const auto use_jobs = job_man.get_thread_count() > 1;
  auto dispatch = [&job_man, use_jobs](const Arc<Barrier>& barrier, auto&& work) {
    if (!use_jobs) {
      work();
      return;
    }

    auto job = Job::create(std::forward<decltype(work)>(work));
    job->signal(barrier);
    job_man.submit(std::move(job));
  };

  auto textures = std::vector<UUID>();
  textures.reserve(data->textures.size());
  for (const auto& texture : data->textures) {
    textures.push_back(texture.uuid.unpack());
  }

  auto texture_barrier = Barrier::create();
  auto texture_batch = UploadBatch::create();
  for (const auto& [texture, texture_uuid] : std::views::zip(data->textures, textures)) {
    if (!texture_uuid) {
      continue;
    }

    dispatch(texture_barrier, [&asset_man = self, texture_uuid, is_srgb = texture.is_srgb, texture_batch]() {
      asset_man.load_asset(texture_uuid, TextureLoadInfo{.is_srgb = is_srgb, .batch = texture_batch.get()}, false);
    });
  }

  texture_barrier->wait(job_man);
  // One fence wait and one vkUpdateDescriptorSets for every texture in the model. Has to happen
  // before the materials below reach the GPU, since they index the slots written here.
  texture_batch->flush(render_context);

  auto materials = std::vector<UUID>();
  materials.reserve(data->materials.size());
  for (const auto& material : data->materials) {
    auto material_uuid = material.uuid.unpack();
    materials.push_back(material_uuid);
    if (!material_uuid) {
      continue;
    }

    self.load_asset(material_uuid, to_material(material, textures), false);
  }

  auto model = Model{};
  model.textures = std::move(textures);
  model.materials = std::move(materials);
  model.default_scene_index = data->default_scene_index;

  model.lights.reserve(data->lights.size());
  for (const auto& light : data->lights) {
    model.lights.push_back({
      .name = light.name,
      .type = static_cast<Model::LightType>(light.type),
      .color = glm::make_vec3(light.color),
      .intensity = light.intensity,
      .range = light.has_range ? option<f32>(light.range) : nullopt,
      .inner_cone_angle = light.has_inner_cone_angle ? option<f32>(light.inner_cone_angle) : nullopt,
      .outer_cone_angle = light.has_outer_cone_angle ? option<f32>(light.outer_cone_angle) : nullopt,
    });
  }

  model.mesh_groups.reserve(data->mesh_groups.size());
  for (const auto& group : data->mesh_groups) {
    auto& mesh_group = model.mesh_groups.emplace_back();
    mesh_group.name = group.name;
    mesh_group.child_indices.assign(group.child_indices.begin(), group.child_indices.end());
    mesh_group.mesh_indices.assign(group.mesh_indices.begin(), group.mesh_indices.end());
    mesh_group.light_indices.assign(group.light_indices.begin(), group.light_indices.end());
    mesh_group.translation = glm::make_vec3(group.translation);
    mesh_group.rotation = glm::quat::wxyz(group.rotation[3], group.rotation[0], group.rotation[1], group.rotation[2]);
    mesh_group.scale = glm::make_vec3(group.scale);
  }

  const auto mesh_count = data->meshes.size();
  model.material_indices.reserve(mesh_count);
  for (const auto& mesh : data->meshes) {
    model.material_indices.push_back(
      mesh.material_index == ModelData::INVALID_INDEX ? option<u32>(nullopt) : option<u32>(mesh.material_index)
    );
  }

  model.gpu_meshes.resize(mesh_count);
  model.lod0_meshlet_counts.assign(mesh_count, 0_u32);
  model.lod0_index_ranges.resize(mesh_count);
  model.gpu_mesh_buffers.resize(mesh_count);
  model.mesh_blases.resize(mesh_count);
  model.collision_meshes.resize(mesh_count);
  model.mesh_ready = std::vector<std::atomic_flag>(mesh_count);
  model.pending_meshes = static_cast<u32>(mesh_count);

  auto model_id = ModelID::Invalid;
  {
    auto write_lock = std::unique_lock(self.models_mutex);
    model_id = self.model_map.create_slot(std::move(model));
  }

  auto mesh_barrier = Barrier::create();
  auto mesh_batch = UploadBatch::create();
  for (auto mesh_index = 0_sz; mesh_index < mesh_count; mesh_index++) {
    dispatch(mesh_barrier, [&asset_man = self, model_id, data, &render_context, mesh_index, mesh_batch]() {
      ZoneScopedN("Mesh Upload");

      const auto& mesh_data = data->meshes[mesh_index];
      auto uploaded = upload_mesh(render_context, mesh_data, mesh_batch.get());

      auto loaded_model = asset_man.get_model(model_id);
      if (!loaded_model) {
        // Still notify, or every waiter on this id blocks forever.
        mesh_batch->flush(render_context);
        asset_man.notify_model_loaded();
        return;
      }

      if (uploaded.buffer) {
        loaded_model->gpu_mesh_buffers[mesh_index] = std::move(uploaded.buffer);
        loaded_model->mesh_blases[mesh_index] = std::move(uploaded.blas);
        loaded_model->gpu_meshes[mesh_index] = uploaded.gpu_mesh;
        loaded_model->collision_meshes[mesh_index] = to_collision_mesh(mesh_data);
        loaded_model->lod0_meshlet_counts[mesh_index] = uploaded.lods[0].meshlet_count;
        loaded_model->lod0_index_ranges[mesh_index] = {uploaded.lods[0].indices, uploaded.lods[0].indices_count};

        loaded_model->mesh_ready[mesh_index].test_and_set(std::memory_order_release);
      }

      auto pending = std::atomic_ref(loaded_model->pending_meshes);
      const auto was_last = pending.fetch_sub(1, std::memory_order_acq_rel) == 1;
      // Lock order is model_load -> models, so drop `models_mutex` before notifying.
      loaded_model.reset();

      if (was_last) {
        // Nothing waits on `mesh_barrier` when async, so the last mesh in has to settle the batch
        // before the model is announced as loaded.
        mesh_batch->flush(render_context);
        asset_man.notify_model_loaded();
      }
    });
  }

  if (!async) {
    mesh_barrier->wait(job_man);
  }

  return model_id;
}

auto AssetManager::load_model(this AssetManager& self, const std::filesystem::path& path, bool async) -> ModelID {
  ZoneScoped;

  auto pack = AssetFile::unpack(path);
  if (!pack) {
    return ModelID::Invalid;
  }

  for (auto& entry : pack->entries) {
    if (auto* model_data = std::get_if<ModelData>(&entry.data)) {
      return self.load_model(std::move(*model_data), async);
    }
  }

  OX_LOG_ERROR("Asset pack '{}' contains no model.", path);

  return ModelID::Invalid;
}

auto AssetManager::unload_model(this AssetManager& self, const ModelID model_id) -> bool {
  ZoneScoped;

  self.wait_until_model_loaded(model_id);

  auto write_lock = std::unique_lock(self.models_mutex);
  if (auto* model = self.model_map.slot(model_id)) {
    *model = Model{};
  }
  self.model_map.destroy_slot(model_id);

  return true;
}
} // namespace ox
