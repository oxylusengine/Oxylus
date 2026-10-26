#include <algorithm>
#include <array>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <utility>
#include <vir/RenderGraph.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/ParticleEmitterProgram.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto PARTICLE_BLEND_STATE = vir::ColorBlendAttachmentState{
  .is_blend_enabled = true,
  .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
  .color_blend_op = VK_BLEND_OP_ADD,
  .src_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
  .alpha_blend_op = VK_BLEND_OP_ADD,
};

using ParticleInitPush = PushConstants<u32>;
using ParticleEmitPush = PushConstants<u32, u32>;
using ParticleDrawArgsPush = PushConstants<u32>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

auto next_power_of_two(u32 value) -> u32 {
  auto result = 1_u32;
  while (result < value) {
    result <<= 1;
  }

  return result;
}

struct ParticleAssetSnapshot {
  ParticleEmitterSettings emitter = {};
  ParticleRenderSettings render = {};
  std::array<glm::vec4, GPU::PARTICLE_USER_PARAM_COUNT> parameter_defaults = {};
  // the emitter program runs on the CPU, so it needs the curve and gradient sources rather than the
  // baked atlas the GPU programs sample
  std::vector<GPU::ParticleInstruction> emitter_instructions = {};
  std::vector<glm::vec4> emitter_constants = {};
  std::vector<ParticleCurve> curves = {};
  std::vector<ParticleGradient> gradients = {};
  bool emitter_consumes_pulse = false;
  u32 instruction_offset = 0;
  u32 constant_offset = 0;
  u32 spawn_count = 0;
  u32 update_count = 0;
  u32 curve_atlas_index = ~0_u32;
  u32 curve_sampler_index = 0;
  u32 curve_row_count = 0;
  u32 atlas_row_count = 0;
};

auto RendererInstance::prepare_particles(this RendererInstance& self, const f32 delta_time, const bool sort_enabled)
  -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto& render_context = *self.renderer.render_context;
  auto& data = self.frame_data;

  auto snapshots = std::vector<ParticleAssetSnapshot>{};
  auto snapshot_of = ankerl::unordered_dense::map<UUID, u32>{};

  auto pool_offset = 0_u32;
  auto layout_changed = false;

  self.scene.world
    .query_builder<const TransformComponent, const ParticleSystemComponent>() //
    .build()
    .each([&](flecs::entity entity, const TransformComponent&, const ParticleSystemComponent& component) {
      const auto emitter_it = self.scene.entity_particle_emitters_map.find(entity);
      if (emitter_it == self.scene.entity_particle_emitters_map.end()) {
        return;
      }

      auto* state = self.scene.particle_emitters.slot(emitter_it->second);
      if (!state || !state->asset) {
        return;
      }

      auto snapshot_it = snapshot_of.find(state->asset);
      if (snapshot_it == snapshot_of.end()) {
        auto snapshot = ParticleAssetSnapshot{};
        {
          auto system = asset_man.get_particle_system(state->asset);
          if (!system || !system->compile_error.empty()) {
            return;
          }

          snapshot.emitter = system->emitter;
          snapshot.render = system->render;
          for (usize i = 0; i < std::min<usize>(system->parameters.size(), GPU::PARTICLE_USER_PARAM_COUNT); i++) {
            snapshot.parameter_defaults[i] = system->parameters[i].default_value;
          }
          snapshot.spawn_count = system->programs.spawn_count;
          snapshot.update_count = system->programs.update_count;
          snapshot.curve_row_count = system->curve_row_count();
          snapshot.atlas_row_count = system->atlas_row_count();
          if (system->curve_atlas) {
            snapshot.curve_atlas_index = system->curve_atlas.get_view_index();
            snapshot.curve_sampler_index = system->curve_atlas.get_sampler_index();
          }

          snapshot.emitter_instructions = system->programs.emitter_instructions;
          snapshot.emitter_constants = system->programs.emitter_constants;
          snapshot.emitter_consumes_pulse = system->programs.emitter_consumes_pulse;
          if (!snapshot.emitter_instructions.empty()) {
            snapshot.curves = system->curves;
            snapshot.gradients = system->gradients;
          }

          snapshot.instruction_offset = static_cast<u32>(data.particle_instructions.size());
          snapshot.constant_offset = static_cast<u32>(data.particle_constants.size());
          data.particle_instructions.insert(
            data.particle_instructions.end(),
            system->programs.instructions.begin(),
            system->programs.instructions.end()
          );
          data.particle_constants.insert(
            data.particle_constants.end(),
            system->programs.constants.begin(),
            system->programs.constants.end()
          );
        }

        snapshots.emplace_back(std::move(snapshot));
        snapshot_it = snapshot_of.emplace(state->asset, static_cast<u32>(snapshots.size() - 1)).first;
      }

      const auto& snapshot = snapshots[snapshot_it->second];
      const auto& settings = snapshot.emitter;
      const auto capacity = std::max(settings.capacity, 1_u32);

      if (!state->pool_valid || state->pool_offset != pool_offset || state->capacity != capacity) {
        layout_changed = true;
      }

      state->pool_offset = pool_offset;
      state->capacity = capacity;
      state->pool_valid = true;
      pool_offset += capacity;

      const auto step = delta_time * std::max(component.simulation_speed, 0.0f);
      state->time += step;

      const auto local_time = state->time - settings.start_delay;
      if (settings.looping && settings.duration > 0.0f && local_time > settings.duration) {
        state->time -= settings.duration;
      }

      auto user_params = std::array<glm::vec4, GPU::PARTICLE_USER_PARAM_COUNT>{};
      for (auto i = 0_u32; i < GPU::PARTICLE_USER_PARAM_COUNT; i++) {
        user_params[i] = component.override_parameters ? component.parameter(i) : snapshot.parameter_defaults[i];
      }

      auto spawn_count = 0_u32;
      const auto within_duration = settings.looping || local_time <= settings.duration;
      if (state->playing && local_time >= 0.0f && within_duration) {
        const auto cycle_time = settings.duration > 0.0f ? std::clamp(local_time / settings.duration, 0.0f, 1.0f)
                                                         : 0.0f;

        const auto emitted = run_particle_emitter_program(
          snapshot.emitter_instructions,
          snapshot.emitter_constants,
          ParticleEmitterProgramInput{
            .time = std::max(local_time, 0.0f),
            .cycle_time = cycle_time,
            .delta_time = step,
            .pulse = static_cast<f32>(state->pending_burst),
            .spawn_rate = settings.spawn_rate,
            .seed = settings.seed ^ component.seed ^ static_cast<u32>(state->time * 1000.0f),
            .user_params = user_params,
            .curves = snapshot.curves,
            .gradients = snapshot.gradients,
          },
          state->program_state
        );

        state->spawn_accumulator += emitted.spawn_rate * std::max(component.emission_rate_scale, 0.0f) * step;
        spawn_count = static_cast<u32>(state->spawn_accumulator);
        state->spawn_accumulator -= static_cast<f32>(spawn_count);
        spawn_count += static_cast<u32>(emitted.spawn + 0.5f);
      }

      // a graph holding a Pulse node has already seen the queued bursts and decided what they mean;
      // without one they land directly, and fire whether or not the emitter is playing
      if (!snapshot.emitter_consumes_pulse) {
        spawn_count += state->pending_burst;
      }
      state->pending_burst = 0;
      spawn_count = std::min(spawn_count, capacity);

      auto material_index = 0_u32;
      if (snapshot.render.material) {
        if (
          auto material = asset_man.get_asset(snapshot.render.material);
          material && material->type == AssetType::Material
        ) {
          material_index = SlotMap_decode_id(material->material_id).index;
        }
      }

      auto flags = GPU::ParticleEmitterFlags::None;
      if (settings.simulation_space == ParticleSimulationSpace::Local) {
        flags |= GPU::ParticleEmitterFlags::LocalSpace;
      }
      if (snapshot.render.blend == ParticleBlendMode::Additive) {
        flags |= GPU::ParticleEmitterFlags::Additive;
      }
      switch (snapshot.render.billboard) {
        case ParticleBillboardMode::VelocityStretched: flags |= GPU::ParticleEmitterFlags::VelocityStretched; break;
        case ParticleBillboardMode::HorizontalPlane  : flags |= GPU::ParticleEmitterFlags::HorizontalPlane; break;
        case ParticleBillboardMode::VerticalPlane    : flags |= GPU::ParticleEmitterFlags::VerticalPlane; break;
        default                                      : break;
      }
      if (snapshot.render.soft_particle_distance > 0.0f) {
        flags |= GPU::ParticleEmitterFlags::SoftParticles;
      }
      if (snapshot.render.depth_collision) {
        flags |= GPU::ParticleEmitterFlags::DepthCollision;
      }

      const auto emitter_index = static_cast<u32>(data.particle_emitters.size());

      if (snapshot.render.render_mode == ParticleRenderMode::Mesh && snapshot.render.mesh) {
        auto mesh_draw = ParticleMeshDraw{.emitter_index = emitter_index};
        auto valid_mesh = false;
        {
          auto model = asset_man.get_model(snapshot.render.mesh);
          if (
            model && !model->gpu_meshes.empty() && !model->lod0_index_ranges.empty() &&
            model->gpu_mesh_buffers[0] != BufferID::Invalid && model->is_mesh_ready(0)
          ) {
            const auto& range = model->lod0_index_ranges[0];
            const auto buffer = render_context.buffer(model->gpu_mesh_buffers[0]);
            if (range.count > 0 && range.device_address >= buffer.device_address) {
              mesh_draw.gpu_mesh = model->gpu_meshes[0];
              mesh_draw.index_count = range.count;
              mesh_draw.index_buffer = buffer;
              mesh_draw.index_buffer.offset += range.device_address - buffer.device_address;
              mesh_draw.index_buffer.device_address = range.device_address;
              mesh_draw.index_buffer.size = static_cast<u64>(range.count) * sizeof(Model::Index);
              valid_mesh = true;
            }
          }
        }

        if (valid_mesh) {
          flags |= GPU::ParticleEmitterFlags::MeshRenderer;
          data.particle_mesh_draws.emplace_back(mesh_draw);
        }
      }

      const auto world = Scene::get_world_transform(entity);

      // spawn direction has to follow the *world* orientation. an exhaust emitter parented to a car
      // inherits the car's rotation, which the entity's local rotation knows nothing about
      auto basis = glm::mat3(world);
      for (auto axis = 0; axis < 3; axis++) {
        const auto axis_length = glm::length(basis[axis]);
        if (axis_length > 1e-6f) {
          basis[axis] /= axis_length;
        } else {
          basis[axis] = glm::vec3(0.0f);
          basis[axis][axis] = 1.0f;
        }
      }
      const auto world_rotation = glm::normalize(glm::quat_cast(basis));

      auto gpu_emitter = GPU::ParticleEmitter{
        .transform = world,
        .rotation = {world_rotation.x, world_rotation.y, world_rotation.z, world_rotation.w},
        .pool_offset = state->pool_offset,
        .capacity = capacity,
        .spawn_count = spawn_count,
        .spawn_offset = data.particle_total_spawn,
        .spawn_program_offset = snapshot.instruction_offset,
        .spawn_program_count = snapshot.spawn_count,
        .update_program_offset = snapshot.instruction_offset + snapshot.spawn_count,
        .update_program_count = snapshot.update_count,
        .constants_offset = snapshot.constant_offset,
        .curve_atlas_index = snapshot.curve_atlas_index,
        .curve_sampler_index = snapshot.curve_sampler_index,
        .curve_row_count = snapshot.curve_row_count,
        .atlas_row_count = snapshot.atlas_row_count,
        .material_index = material_index,
        .flipbook_x = std::max(snapshot.render.flipbook.x, 1_u32),
        .flipbook_y = std::max(snapshot.render.flipbook.y, 1_u32),
        .flags = flags,
        .shape = settings.shape,
        .seed = settings.seed ^ component.seed ^ (emitter_index * 2654435761_u32),
        .time = state->time,
        .delta_time = step,
        .restitution = snapshot.render.restitution,
        .soft_particle_distance = snapshot.render.soft_particle_distance,
        .velocity_stretch = snapshot.render.velocity_stretch,
        .lifetime = settings.lifetime,
        .shape_params = {settings.shape_size, settings.shape_angle},
        .velocity_offset = glm::vec4(component.velocity_offset, 0.0f),
      };

      std::ranges::copy(user_params, gpu_emitter.user_params);

      data.particle_total_spawn += spawn_count;
      data.particle_sort_enabled = data.particle_sort_enabled || snapshot.render.sort;
      data.particle_emitters.emplace_back(gpu_emitter);
    });

  if (data.particle_emitters.empty()) {
    return;
  }

  if (data.particle_instructions.empty()) {
    data.particle_instructions.emplace_back();
  }
  if (data.particle_constants.empty()) {
    data.particle_constants.emplace_back(0.0f);
  }

  data.particle_total_capacity = pool_offset;
  data.particle_sort_enabled = data.particle_sort_enabled && sort_enabled;

  if (pool_offset != self.particle_pool_capacity) {
    layout_changed = true;
  }

  const auto sorted_count = next_power_of_two(std::max(pool_offset, GPU::PARTICLE_SORT_GROUP));

  const auto resize = [&](BufferID& id, u64 size, std::string_view name) {
    if (const auto resized = render_context.resize_buffer(id, size, name); !resized) {
      OX_LOG_ERROR("failed to allocate {}", name);
    }
  };
  resize(self.particle_buffer, static_cast<u64>(pool_offset) * sizeof(GPU::Particle), "particles");
  resize(self.particle_alive_list_a, static_cast<u64>(pool_offset) * sizeof(u32), "particle alive list a");
  resize(self.particle_alive_list_b, static_cast<u64>(pool_offset) * sizeof(u32), "particle alive list b");
  resize(self.particle_dead_list, static_cast<u64>(pool_offset) * sizeof(u32), "particle dead list");
  resize(
    self.particle_dead_counts,
    static_cast<u64>(data.particle_emitters.size()) * sizeof(u32),
    "particle dead counts"
  );
  resize(self.particle_counters, sizeof(GPU::ParticleCounters), "particle counters");
  resize(self.particle_sort_keys, static_cast<u64>(sorted_count) * sizeof(GPU::ParticleSortKey), "particle sort keys");

  data.particle_meshes.resize(std::max<usize>(data.particle_mesh_draws.size(), 1));
  data.particle_index_counts.resize(std::max<usize>(data.particle_mesh_draws.size(), 1));
  for (usize i = 0; i < data.particle_mesh_draws.size(); i++) {
    data.particle_meshes[i] = data.particle_mesh_draws[i].gpu_mesh;
    data.particle_index_counts[i] = data.particle_mesh_draws[i].index_count;
  }

  data.particle_ping = self.particle_pool_ping;
  self.particle_pool_ping = !self.particle_pool_ping;
  self.particle_pool_capacity = pool_offset;
  data.particle_sorted_count = sorted_count;
  data.particle_pool_reset = layout_changed;
}

auto RendererInstance::bind_particle_variables(
  this const RendererInstance& self, vir::Program& program, const ParticleVariables& variables
) -> void {
  ZoneScoped;

  if (!self.frame_params.shape.has_particles) {
    return;
  }

  auto& render_context = *self.renderer.render_context;
  const auto& data = self.frame_data;
  program.set(variables.particles, render_context.buffer(self.particle_buffer));
  program.set(variables.dead_list, render_context.buffer(self.particle_dead_list));
  program.set(variables.dead_counts, render_context.buffer(self.particle_dead_counts));
  program.set(variables.counters, render_context.buffer(self.particle_counters));
  program.set(variables.sort_keys, render_context.buffer(self.particle_sort_keys));
  const auto is_ping = data.particle_ping;
  program.set(
    variables.alive_in,
    render_context.buffer(!is_ping ? self.particle_alive_list_a : self.particle_alive_list_b)
  );
  program.set(
    variables.alive_out,
    render_context.buffer(is_ping ? self.particle_alive_list_a : self.particle_alive_list_b)
  );

  program.set(variables.total_capacity, data.particle_total_capacity);
  program.set(variables.total_spawn, data.particle_total_spawn);
  program.set(variables.is_pool_reset, data.particle_pool_reset);
  program.set_bytes(variables.init_push, ParticleInitPush(data.particle_total_capacity));
  program.set_bytes(
    variables.emit_push,
    ParticleEmitPush(data.particle_total_spawn, static_cast<u32>(data.particle_emitters.size()))
  );
  program.set_bytes(
    variables.mesh_draw_push,
    ParticleDrawArgsPush(std::min(static_cast<u32>(data.particle_mesh_draws.size()), ParticleContext::MAX_MESH_DRAWS))
  );
}

auto RendererInstance::simulate_particles(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, ParticleContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;
  memory::ScopedStack stack;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.particle;
  variables.index_counts = module.declare_buffer_var("particle index counts", vir::Access::None);
  variables.particles = module.declare_buffer_var("particles", vir::Access::ComputeRW);
  variables.dead_list = module.declare_buffer_var("particle dead list", vir::Access::ComputeRW);
  variables.dead_counts = module.declare_buffer_var("particle dead counts", vir::Access::ComputeRW);
  variables.counters = module.declare_buffer_var("particle counters", vir::Access::ComputeRW);
  variables.sort_keys = module.declare_buffer_var("particle sort keys", vir::Access::ComputeRW);
  variables.alive_in = module.declare_buffer_var("particle alive in", vir::Access::ComputeRW);
  variables.alive_out = module.declare_buffer_var("particle alive out", vir::Access::ComputeRW);
  variables.total_capacity = module.declare_u32_var("particle capacity", 0);
  variables.total_spawn = module.declare_u32_var("particle spawn", 0);
  variables.is_pool_reset = module.declare_bool_var("particle pool reset", false);
  variables.init_push = module.declare_bytes_var("particle init push", sizeof(ParticleInitPush));
  variables.emit_push = module.declare_bytes_var("particle emit push", sizeof(ParticleEmitPush));
  variables.mesh_draw_push = module.declare_bytes_var("particle build draw args push", sizeof(ParticleDrawArgsPush));

  auto particles = variables.particles;
  auto dead_list = variables.dead_list;
  auto dead_counts = variables.dead_counts;
  auto counters = variables.counters;
  auto sort_keys = variables.sort_keys;
  auto alive_in = variables.alive_in;
  auto alive_out = variables.alive_out;

  const auto total_capacity = variables.total_capacity;
  const auto total_spawn = variables.total_spawn;

  // a pool whose layout changed starts over, every slot dead and every counter zeroed
  const auto write_initial_counts = module.declare_callback_var(
    "particle initial counts",
    [&self, dead_counts, counters](vir::Recorder& recorder) -> void {
      memory::ScopedStack callback_stack;
      const auto& emitters = self.frame_data.particle_emitters;
      auto initial = callback_stack.alloc<u32>(std::max<usize>(emitters.size(), 1));
      initial[0] = 0;
      for (usize i = 0; i < emitters.size(); i++) {
        initial[i] = emitters[i].capacity;
      }

      const auto zeroed = GPU::ParticleCounters{};
      recorder
        .update_buffer(
          recorder.buffer(dead_counts),
          std::span(reinterpret_cast<const u8*>(initial.data()), initial.size_bytes())
        )
        .update_buffer(recorder.buffer(counters), std::span(reinterpret_cast<const u8*>(&zeroed), sizeof(zeroed)));
    }
  );
  std::tie(dead_list, particles, dead_counts, counters) = std::tuple_cat(module.set_condition(
    variables.is_pool_reset,
    [&](vir::Module& arm) {
      const auto [counts, zeroed] = //
        arm.begin_compute({{dead_counts, vir::Access::Clear}, {counters, vir::Access::Clear}})
          .with_name("particle initial counts")
          .record_from(write_initial_counts)
          .end_compute();

      const auto [initial_dead_list, initial_particles] =
        arm.begin_compute({{dead_list, vir::Access::ComputeWrite}, {particles, vir::Access::ComputeWrite}})
          .with_name("particle init")
          .bind_compute_pipeline(render_context.pipeline("particle_init"))
          .bind_buffer(0, 0, dead_list)
          .bind_buffer(0, 1, particles)
          .push_constants_from(variables.init_push)
          .dispatch_invocations(total_capacity, 1, 1)
          .end_compute();
      return std::array{initial_dead_list, initial_particles, counts, zeroed};
    },
    [&](vir::Module&) { return std::array{dead_list, particles, dead_counts, counters}; }
  ));

  counters = //
    module.begin_compute({{counters, vir::Access::ComputeRW}})
      .with_name("particle prepare")
      .bind_compute_pipeline(render_context.pipeline("particle_prepare"))
      .bind_buffer(0, 0, counters)
      .dispatch(1, 1, 1)
      .end_compute()[0];

  std::tie(
    particles,
    dead_list,
    dead_counts,
    alive_in,
    counters,
    context.emitters_buffer,
    context.program_buffer,
    context.constants_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{particles, vir::Access::ComputeRW},
         {dead_list, vir::Access::ComputeRW},
         {dead_counts, vir::Access::ComputeRW},
         {alive_in, vir::Access::ComputeRW},
         {counters, vir::Access::ComputeRW},
         {context.emitters_buffer, vir::Access::ComputeRead},
         {context.program_buffer, vir::Access::ComputeRead},
         {context.constants_buffer, vir::Access::ComputeRead}}
      )
      .with_name("particle emit")
      .bind_compute_pipeline(render_context.pipeline("particle_emit"))
      .bind_buffer(0, 0, particles)
      .bind_buffer(0, 1, dead_list)
      .bind_buffer(0, 2, dead_counts)
      .bind_buffer(0, 3, alive_in)
      .bind_buffer(0, 4, counters)
      .bind_buffer(0, 5, context.emitters_buffer)
      .bind_buffer(0, 6, context.program_buffer)
      .bind_buffer(0, 7, context.constants_buffer)
      .push_constants_from(variables.emit_push)
      .dispatch_invocations(total_spawn, 1, 1)
      .end_compute()
  );

  const auto simulate_args_value = VkDispatchIndirectCommand{.x = 0, .y = 1, .z = 1};
  auto simulate_args = module.transient_buffer({
    .size = sizeof(simulate_args_value),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "particle simulate args",
  });
  simulate_args = module.update_buffer(
    simulate_args,
    std::span{reinterpret_cast<const u8*>(&simulate_args_value), sizeof(simulate_args_value)}
  );
  std::tie(counters, simulate_args) = std::tuple_cat(
    module.begin_compute({{counters, vir::Access::ComputeRead}, {simulate_args, vir::Access::ComputeWrite}})
      .with_name("particle build dispatch args")
      .bind_compute_pipeline(render_context.pipeline("particle_build_dispatch_args"))
      .bind_buffer(0, 0, counters)
      .bind_buffer(0, 1, simulate_args)
      .dispatch(1, 1, 1)
      .end_compute()
  );

  std::tie(
    std::ignore,
    particles,
    alive_in,
    alive_out,
    dead_list,
    dead_counts,
    counters,
    context.emitters_buffer,
    context.program_buffer,
    context.constants_buffer,
    sort_keys,
    values.camera,
    context.depth_attachment
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{simulate_args, vir::Access::IndirectRead},
         {particles, vir::Access::ComputeRW},
         {alive_in, vir::Access::ComputeRead},
         {alive_out, vir::Access::ComputeWrite},
         {dead_list, vir::Access::ComputeRW},
         {dead_counts, vir::Access::ComputeRW},
         {counters, vir::Access::ComputeRW},
         {context.emitters_buffer, vir::Access::ComputeRead},
         {context.program_buffer, vir::Access::ComputeRead},
         {context.constants_buffer, vir::Access::ComputeRead},
         {sort_keys, vir::Access::ComputeWrite},
         {values.camera, vir::Access::ComputeUniformRead},
         {context.depth_attachment, vir::Access::ComputeSampled}}
      )
      .with_name("particle simulate")
      .bind_compute_pipeline(render_context.pipeline("particle_simulate"))
      .bind_buffer(0, 0, particles)
      .bind_buffer(0, 1, alive_in)
      .bind_buffer(0, 2, alive_out)
      .bind_buffer(0, 3, dead_list)
      .bind_buffer(0, 4, dead_counts)
      .bind_buffer(0, 5, counters)
      .bind_buffer(0, 6, context.emitters_buffer)
      .bind_buffer(0, 7, context.program_buffer)
      .bind_buffer(0, 8, context.constants_buffer)
      .bind_buffer(0, 9, sort_keys)
      .bind_buffer(0, 10, values.camera)
      .bind_image(0, 11, context.depth_attachment)
      .dispatch_indirect(simulate_args)
      .end_compute()
  );

  // the pool size is part of the frame shape, so the sort can be unrolled here
  const auto sorted_count = recording.shape.particle_sorted_count;
  std::tie(sort_keys, counters) = std::tuple_cat(
    module.begin_compute({{sort_keys, vir::Access::ComputeRW}, {counters, vir::Access::ComputeRead}})
      .with_name("particle sort local")
      .bind_compute_pipeline(render_context.pipeline("particle_sort_local"))
      .bind_buffer(0, 0, sort_keys)
      .bind_buffer(0, 1, counters)
      .push_constants(PushConstants(sorted_count))
      .dispatch_invocations(sorted_count, 1, 1)
      .end_compute()
  );

  if (recording.shape.is_particle_sort) {
    for (auto k = GPU::PARTICLE_SORT_GROUP * 2; k <= sorted_count; k <<= 1) {
      for (auto j = k >> 1; j >= GPU::PARTICLE_SORT_GROUP; j >>= 1) {
        sort_keys = //
          module.begin_compute({{sort_keys, vir::Access::ComputeRW}})
            .with_name(stack.format("particle sort {}/{}", k, j))
            .bind_compute_pipeline(render_context.pipeline("particle_sort_step"))
            .bind_buffer(0, 0, sort_keys)
            .push_constants(PushConstants(sorted_count, k, j))
            .dispatch_invocations(sorted_count, 1, 1)
            .end_compute()[0];
      }

      sort_keys = //
        module.begin_compute({{sort_keys, vir::Access::ComputeRW}})
          .with_name(stack.format("particle sort merge {}", k))
          .bind_compute_pipeline(render_context.pipeline("particle_sort_merge"))
          .bind_buffer(0, 0, sort_keys)
          .push_constants(PushConstants(sorted_count, k))
          .dispatch_invocations(sorted_count, 1, 1)
          .end_compute()[0];
    }
  }

  const auto
    draw_command = VkDrawIndirectCommand{.vertexCount = 6, .instanceCount = 0, .firstVertex = 0, .firstInstance = 0};
  context.draw_cmd_buffer = module.transient_buffer({
    .size = sizeof(draw_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "particle draw command",
  });
  context.draw_cmd_buffer = module.update_buffer(
    context.draw_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&draw_command), sizeof(draw_command)}
  );
  auto index_counts = variables.index_counts;
  context.draw_indexed_cmd_buffer = module.transient_buffer({
    .size = sizeof(VkDrawIndexedIndirectCommand) * ParticleContext::MAX_MESH_DRAWS,
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "particle draw indexed commands",
  });
  std::tie(counters, context.draw_cmd_buffer, context.draw_indexed_cmd_buffer, std::ignore) = std::tuple_cat(
    module
      .begin_compute(
        {{counters, vir::Access::ComputeRead},
         {context.draw_cmd_buffer, vir::Access::ComputeWrite},
         {context.draw_indexed_cmd_buffer, vir::Access::ComputeWrite},
         {index_counts, vir::Access::ComputeRead}}
      )
      .with_name("particle build draw args")
      .bind_compute_pipeline(render_context.pipeline("particle_build_draw_args"))
      .bind_buffer(0, 0, counters)
      .bind_buffer(0, 1, context.draw_cmd_buffer)
      .bind_buffer(0, 2, context.draw_indexed_cmd_buffer)
      .bind_buffer(0, 3, index_counts)
      .push_constants_from(variables.mesh_draw_push)
      .dispatch(1, 1, 1)
      .end_compute()
  );

  // the draws only read the particles and their order, everything else is done for this frame
  module.export_to(alive_in, vir::Access::ComputeRW);
  module.export_to(alive_out, vir::Access::ComputeRW);
  module.export_to(dead_list, vir::Access::ComputeRW);
  module.export_to(dead_counts, vir::Access::ComputeRW);
  module.export_to(counters, vir::Access::ComputeRW);

  context.particles_buffer = particles;
  context.sort_keys_buffer = sort_keys;
}

auto RendererInstance::draw_particles(
  this RendererInstance& self,
  vir::Module& module,
  FrameRecording& recording,
  ParticleContext& context,
  vir::ValueID dst
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.particle;
  variables.meshes = module.declare_buffer_var("particle meshes", vir::Access::None);

  std::tie(
    dst,
    context.reactive_mask_attachment,
    std::ignore,
    context.sort_keys_buffer,
    context.particles_buffer,
    context.emitters_buffer,
    values.camera,
    values.materials,
    context.depth_attachment
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{dst, vir::Access::ColorRW},
         {context.reactive_mask_attachment, vir::Access::ColorRW},
         {context.draw_cmd_buffer, vir::Access::IndirectRead},
         {context.sort_keys_buffer, vir::Access::VertexRead},
         {context.particles_buffer, vir::Access::VertexRead},
         {context.emitters_buffer, vir::Access::VertexRead},
         {values.camera, vir::Access::VertexUniformRead | vir::Access::FragmentUniformRead},
         {values.materials, vir::Access::FragmentRead},
         {context.depth_attachment, vir::Access::FragmentSampled}}
      )
      .with_name("particle billboards")
      .bind_graphics_pipeline(render_context.pipeline("particle_billboard"))
      .set_color_blend(0, PARTICLE_BLEND_STATE)
      .set_color_blend(1, reactive_mask_blend_state())
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_buffer(0, 0, context.sort_keys_buffer)
      .bind_buffer(0, 1, context.particles_buffer)
      .bind_buffer(0, 2, context.emitters_buffer)
      .bind_buffer(0, 3, values.camera)
      .bind_image(0, 4, context.depth_attachment)
      .push_constant_address(0, values.materials)
      .draw_indirect(context.draw_cmd_buffer, 1)
      .end_rendering()
  );

  // how many emitters draw a mesh, and which, changes every frame
  auto meshes = prepared.particle.meshes;
  const auto materials = values.materials;
  const auto draw_commands = context.draw_indexed_cmd_buffer;
  const auto mesh_draws = module.declare_callback_var(
    "particle meshes",
    [&self, materials, meshes, draw_commands](vir::Recorder& recorder) -> void {
      const auto& draws = self.frame_data.particle_mesh_draws;
      const auto materials_address = recorder.buffer(materials).device_address;
      const auto meshes_buffer = recorder.buffer(meshes);
      const auto commands = recorder.buffer(draw_commands);
      for (auto draw_index = 0_u32; draw_index < draws.size(); draw_index++) {
        const auto& draw = draws[draw_index];
        if (draw.index_count == 0 || draw_index >= ParticleContext::MAX_MESH_DRAWS) {
          continue;
        }

        recorder.bind_index_buffer(draw.index_buffer, VK_INDEX_TYPE_UINT32)
          .push_constants(PushConstants(
            materials_address,
            meshes_buffer.device_address + draw_index * sizeof(GPU::Mesh),
            draw.emitter_index
          ))
          .draw_indexed_indirect(commands, draw_index * sizeof(VkDrawIndexedIndirectCommand), 1);
      }
    }
  );
  std::tie(
    dst,
    context.reactive_mask_attachment,
    context.draw_indexed_cmd_buffer,
    context.sort_keys_buffer,
    context.particles_buffer,
    context.emitters_buffer,
    values.camera,
    std::ignore,
    values.materials,
    context.depth_attachment
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{dst, vir::Access::ColorRW},
         {context.reactive_mask_attachment, vir::Access::ColorRW},
         {context.draw_indexed_cmd_buffer, vir::Access::IndirectRead},
         {context.sort_keys_buffer, vir::Access::VertexRead},
         {context.particles_buffer, vir::Access::VertexRead},
         {context.emitters_buffer, vir::Access::VertexRead},
         {values.camera, vir::Access::VertexUniformRead | vir::Access::FragmentUniformRead},
         {meshes, vir::Access::VertexRead},
         {values.materials, vir::Access::FragmentRead},
         {context.depth_attachment, vir::Access::FragmentSampled}}
      )
      .with_name("particle meshes")
      .bind_graphics_pipeline(render_context.pipeline("particle_mesh"))
      .set_rasterization({.cull_mode = VK_CULL_MODE_BACK_BIT})
      .set_color_blend(0, PARTICLE_BLEND_STATE)
      .set_color_blend(1, reactive_mask_blend_state())
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_buffer(0, 0, context.sort_keys_buffer)
      .bind_buffer(0, 1, context.particles_buffer)
      .bind_buffer(0, 2, context.emitters_buffer)
      .bind_buffer(0, 3, values.camera)
      .bind_image(0, 4, context.depth_attachment)
      .record_from(mesh_draws)
      .end_rendering()
  );

  module.export_to(context.particles_buffer, vir::Access::ComputeRW);
  module.export_to(context.sort_keys_buffer, vir::Access::ComputeRW);
  return dst;
}
} // namespace ox
