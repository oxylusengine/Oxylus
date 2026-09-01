#include <array>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <utility>
#include <vuk/runtime/CommandBuffer.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/ParticleEmitterProgram.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/Scene.hpp"

namespace ox {
constexpr static auto PARTICLE_BLEND_STATE = vuk::PipelineColorBlendAttachmentState{
  .blendEnable = true,
  .srcColorBlendFactor = vuk::BlendFactor::eOne,
  .dstColorBlendFactor = vuk::BlendFactor::eOneMinusSrcAlpha,
  .colorBlendOp = vuk::BlendOp::eAdd,
  .srcAlphaBlendFactor = vuk::BlendFactor::eOne,
  .dstAlphaBlendFactor = vuk::BlendFactor::eOneMinusSrcAlpha,
  .alphaBlendOp = vuk::BlendOp::eAdd,
};

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
  auto& prepared = self.prepared_frame;

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

          snapshot.instruction_offset = static_cast<u32>(prepared.particle_instructions.size());
          snapshot.constant_offset = static_cast<u32>(prepared.particle_constants.size());
          prepared.particle_instructions.insert(
            prepared.particle_instructions.end(),
            system->programs.instructions.begin(),
            system->programs.instructions.end()
          );
          prepared.particle_constants.insert(
            prepared.particle_constants.end(),
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

      const auto emitter_index = static_cast<u32>(prepared.particle_emitters.size());

      if (snapshot.render.render_mode == ParticleRenderMode::Mesh && snapshot.render.mesh) {
        auto mesh_draw = ParticleMeshDraw{.emitter_index = emitter_index};
        auto valid_mesh = false;
        {
          auto model = asset_man.get_model(snapshot.render.mesh);
          if (
            model && !model->gpu_meshes.empty() && !model->lod0_index_ranges.empty() && model->gpu_mesh_buffers[0] &&
            model->is_mesh_ready(0)
          ) {
            const auto& range = model->lod0_index_ranges[0];
            const auto& buffer = *model->gpu_mesh_buffers[0];
            if (range.count > 0 && range.device_address >= buffer.device_address) {
              mesh_draw.gpu_mesh = model->gpu_meshes[0];
              mesh_draw.index_count = range.count;
              mesh_draw.index_buffer = buffer.subrange(
                range.device_address - buffer.device_address,
                static_cast<u64>(range.count) * sizeof(Model::Index)
              );
              valid_mesh = true;
            }
          }
        }

        if (valid_mesh) {
          flags |= GPU::ParticleEmitterFlags::MeshRenderer;
          prepared.particle_mesh_draws.emplace_back(mesh_draw);
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
        .spawn_offset = prepared.particle_total_spawn,
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
        .shape = std::to_underlying(settings.shape),
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

      gpu_emitter.user_params = user_params;

      prepared.particle_total_spawn += spawn_count;
      prepared.particle_sort_enabled = prepared.particle_sort_enabled || snapshot.render.sort;
      prepared.particle_emitters.emplace_back(gpu_emitter);
    });

  if (prepared.particle_emitters.empty()) {
    return;
  }

  if (prepared.particle_instructions.empty()) {
    prepared.particle_instructions.emplace_back();
  }
  if (prepared.particle_constants.empty()) {
    prepared.particle_constants.emplace_back(0.0f);
  }

  prepared.particle_total_capacity = pool_offset;
  prepared.particle_sort_enabled = prepared.particle_sort_enabled && sort_enabled;

  if (pool_offset != self.particle_pool_capacity) {
    layout_changed = true;
  }

  const auto sorted_count = next_power_of_two(std::max(pool_offset, GPU::PARTICLE_SORT_GROUP));

  self.particle_buffer = render_context.resize_buffer(
    std::move(self.particle_buffer),
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(pool_offset) * sizeof(GPU::Particle)
  );
  self.particle_alive_list_a = render_context.resize_buffer(
    std::move(self.particle_alive_list_a),
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(pool_offset) * sizeof(u32)
  );
  self.particle_alive_list_b = render_context.resize_buffer(
    std::move(self.particle_alive_list_b),
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(pool_offset) * sizeof(u32)
  );
  self.particle_dead_list = render_context.resize_buffer(
    std::move(self.particle_dead_list),
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(pool_offset) * sizeof(u32)
  );
  self.particle_dead_counts = render_context.resize_buffer(
    std::move(self.particle_dead_counts),
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(prepared.particle_emitters.size()) * sizeof(u32)
  );
  self.particle_counters = render_context.resize_buffer(
    std::move(self.particle_counters),
    vuk::MemoryUsage::eGPUonly,
    sizeof(GPU::ParticleCounters)
  );
  self.particle_sort_keys = render_context.resize_buffer(
    std::move(self.particle_sort_keys),
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(sorted_count) * sizeof(GPU::ParticleSortKey)
  );

  self.particle_pool_capacity = pool_offset;
  prepared.particle_sorted_count = sorted_count;
  prepared.particle_pool_reset = layout_changed;
}

auto RendererInstance::simulate_particles(this RendererInstance& self, ParticleContext& context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = *self.renderer.render_context;

  auto& alive_in = self.particle_pool_ping ? self.particle_alive_list_b : self.particle_alive_list_a;
  auto& alive_out = self.particle_pool_ping ? self.particle_alive_list_a : self.particle_alive_list_b;

  auto particles_buffer = vuk::acquire_buf("particles", *self.particle_buffer, vuk::eMemoryRead);
  auto dead_list_buffer = vuk::acquire_buf("particle dead list", *self.particle_dead_list, vuk::eMemoryRead);
  auto alive_in_buffer = vuk::acquire_buf("particle alive in", *alive_in, vuk::eMemoryRead);
  auto alive_out_buffer = vuk::acquire_buf("particle alive out", *alive_out, vuk::eNone);
  auto sort_keys_buffer = vuk::acquire_buf("particle sort keys", *self.particle_sort_keys, vuk::eNone);

  auto dead_counts_buffer = vuk::Value<vuk::Buffer>{};
  auto counters_buffer = vuk::Value<vuk::Buffer>{};

  if (context.needs_init) {
    auto dead_counts = stack.alloc<u32>(context.emitter_count);
    for (auto i = 0_u32; i < context.emitter_count; i++) {
      dead_counts[i] = self.prepared_frame.particle_emitters[i].capacity;
    }

    auto counters = stack.alloc<GPU::ParticleCounters>(1);
    counters[0] = {};

    dead_counts_buffer = render_context.upload_staging(dead_counts, *self.particle_dead_counts);
    counters_buffer = render_context.upload_staging(counters, *self.particle_counters);

    auto init_pass = vuk::make_pass(
      "particle init",
      [total_capacity = context.total_capacity](
        vuk::CommandBuffer& cmd_list, //
        VUK_BA(vuk::eComputeWrite) dead_list,
        VUK_BA(vuk::eComputeWrite) particles
      ) {
        cmd_list //
          .bind_compute_pipeline("particle_init")
          .bind_buffer(0, 0, dead_list)
          .bind_buffer(0, 1, particles)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(total_capacity))
          .dispatch_invocations(total_capacity);

        return std::make_tuple(dead_list, particles);
      }
    );

    std::tie(dead_list_buffer, particles_buffer) = init_pass(std::move(dead_list_buffer), std::move(particles_buffer));
  } else {
    dead_counts_buffer = vuk::acquire_buf("particle dead counts", *self.particle_dead_counts, vuk::eMemoryRead);
    counters_buffer = vuk::acquire_buf("particle counters", *self.particle_counters, vuk::eMemoryRead);
  }

  auto prepare_pass = vuk::make_pass(
    "particle prepare",
    [](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eComputeRW) counters) {
      cmd_list //
        .bind_compute_pipeline("particle_prepare")
        .bind_buffer(0, 0, counters)
        .dispatch(1);

      return counters;
    }
  );

  counters_buffer = prepare_pass(std::move(counters_buffer));

  if (context.total_spawn > 0) {
    auto emit_pass = vuk::make_pass(
      "particle emit",
      [total_spawn = context.total_spawn, emitter_count = context.emitter_count](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eComputeRW) particles,
        VUK_BA(vuk::eComputeRW) dead_list,
        VUK_BA(vuk::eComputeRW) dead_counts,
        VUK_BA(vuk::eComputeRW) alive_list,
        VUK_BA(vuk::eComputeRW) counters,
        VUK_BA(vuk::eComputeRead) emitters,
        VUK_BA(vuk::eComputeRead) program,
        VUK_BA(vuk::eComputeRead) constants
      ) {
        cmd_list //
          .bind_compute_pipeline("particle_emit")
          .bind_buffer(0, 0, particles)
          .bind_buffer(0, 1, dead_list)
          .bind_buffer(0, 2, dead_counts)
          .bind_buffer(0, 3, alive_list)
          .bind_buffer(0, 4, counters)
          .bind_buffer(0, 5, emitters)
          .bind_buffer(0, 6, program)
          .bind_buffer(0, 7, constants)
          .bind_persistent(1, App::get_rendercontext().get_descriptor_set())
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(total_spawn, emitter_count))
          .dispatch_invocations(total_spawn);

        return std::make_tuple(particles, dead_list, dead_counts, alive_list, counters, emitters, program, constants);
      }
    );

    std::tie(
      particles_buffer,
      dead_list_buffer,
      dead_counts_buffer,
      alive_in_buffer,
      counters_buffer,
      context.emitters_buffer,
      context.program_buffer,
      context.constants_buffer
    ) =
      emit_pass(
        std::move(particles_buffer),
        std::move(dead_list_buffer),
        std::move(dead_counts_buffer),
        std::move(alive_in_buffer),
        std::move(counters_buffer),
        std::move(context.emitters_buffer),
        std::move(context.program_buffer),
        std::move(context.constants_buffer)
      );
  }

  auto simulate_args_buffer = render_context.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

  auto build_args_pass = vuk::make_pass(
    "particle build dispatch args",
    [](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eComputeRead) counters, VUK_BA(vuk::eComputeWrite) simulate_args) {
      cmd_list //
        .bind_compute_pipeline("particle_build_dispatch_args")
        .bind_buffer(0, 0, counters)
        .bind_buffer(0, 1, simulate_args)
        .dispatch(1);

      return std::make_tuple(counters, simulate_args);
    }
  );

  std::tie(counters_buffer, simulate_args_buffer) = build_args_pass(
    std::move(counters_buffer),
    std::move(simulate_args_buffer)
  );

  auto simulate_pass = vuk::make_pass(
    "particle simulate",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) simulate_args,
      VUK_BA(vuk::eComputeRW) particles,
      VUK_BA(vuk::eComputeRead) alive_list_in,
      VUK_BA(vuk::eComputeWrite) alive_list_out,
      VUK_BA(vuk::eComputeRW) dead_list,
      VUK_BA(vuk::eComputeRW) dead_counts,
      VUK_BA(vuk::eComputeRW) counters,
      VUK_BA(vuk::eComputeRead) emitters,
      VUK_BA(vuk::eComputeRead) program,
      VUK_BA(vuk::eComputeRead) constants,
      VUK_BA(vuk::eComputeWrite) sort_keys,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_IA(vuk::eComputeSampled) depth
    ) {
      cmd_list //
        .bind_compute_pipeline("particle_simulate")
        .bind_buffer(0, 0, particles)
        .bind_buffer(0, 1, alive_list_in)
        .bind_buffer(0, 2, alive_list_out)
        .bind_buffer(0, 3, dead_list)
        .bind_buffer(0, 4, dead_counts)
        .bind_buffer(0, 5, counters)
        .bind_buffer(0, 6, emitters)
        .bind_buffer(0, 7, program)
        .bind_buffer(0, 8, constants)
        .bind_buffer(0, 9, sort_keys)
        .bind_buffer(0, 10, camera)
        .bind_image(0, 11, depth)
        .bind_persistent(1, App::get_rendercontext().get_descriptor_set())
        .dispatch_indirect(simulate_args);

      return std::make_tuple(
        particles,
        alive_list_in,
        alive_list_out,
        dead_list,
        dead_counts,
        counters,
        emitters,
        program,
        constants,
        sort_keys,
        camera,
        depth
      );
    }
  );

  std::tie(
    particles_buffer,
    alive_in_buffer,
    alive_out_buffer,
    dead_list_buffer,
    dead_counts_buffer,
    counters_buffer,
    context.emitters_buffer,
    context.program_buffer,
    context.constants_buffer,
    sort_keys_buffer,
    context.camera_buffer,
    context.depth_attachment
  ) =
    simulate_pass(
      std::move(simulate_args_buffer),
      std::move(particles_buffer),
      std::move(alive_in_buffer),
      std::move(alive_out_buffer),
      std::move(dead_list_buffer),
      std::move(dead_counts_buffer),
      std::move(counters_buffer),
      std::move(context.emitters_buffer),
      std::move(context.program_buffer),
      std::move(context.constants_buffer),
      std::move(sort_keys_buffer),
      std::move(context.camera_buffer),
      std::move(context.depth_attachment)
    );

  // --- Bitonic sort ---
  const auto sorted_count = context.sorted_count;

  auto sort_local_pass = vuk::make_pass(
    "particle sort local",
    [sorted_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRW) sort_keys,
      VUK_BA(vuk::eComputeRead) counters
    ) {
      cmd_list //
        .bind_compute_pipeline("particle_sort_local")
        .bind_buffer(0, 0, sort_keys)
        .bind_buffer(0, 1, counters)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(sorted_count))
        .dispatch_invocations(sorted_count);

      return std::make_tuple(sort_keys, counters);
    }
  );

  std::tie(sort_keys_buffer, counters_buffer) = sort_local_pass(
    std::move(sort_keys_buffer),
    std::move(counters_buffer)
  );

  if (context.sort_enabled) {
    for (auto k = GPU::PARTICLE_SORT_GROUP * 2; k <= sorted_count; k <<= 1) {
      for (auto j = k >> 1; j >= GPU::PARTICLE_SORT_GROUP; j >>= 1) {
        auto step_pass = vuk::make_pass(
          stack.format("particle sort {}/{}", k, j),
          [sorted_count, k, j](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eComputeRW) sort_keys) {
            cmd_list //
              .bind_compute_pipeline("particle_sort_step")
              .bind_buffer(0, 0, sort_keys)
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(sorted_count, k, j))
              .dispatch_invocations(sorted_count);

            return sort_keys;
          }
        );

        sort_keys_buffer = step_pass(std::move(sort_keys_buffer));
      }

      auto merge_pass = vuk::make_pass(
        stack.format("particle sort merge {}", k),
        [sorted_count, k](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eComputeRW) sort_keys) {
          cmd_list //
            .bind_compute_pipeline("particle_sort_merge")
            .bind_buffer(0, 0, sort_keys)
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(sorted_count, k))
            .dispatch_invocations(sorted_count);

          return sort_keys;
        }
      );

      sort_keys_buffer = merge_pass(std::move(sort_keys_buffer));
    }
  }

  auto draw_cmd_buffer = render_context.scratch_buffer<vuk::DrawIndirectCommand>(
    {.vertexCount = 6, .instanceCount = 0, .firstVertex = 0, .firstInstance = 0}
  );

  const auto mesh_draw_count = static_cast<u32>(context.mesh_draws.size());
  auto draw_indexed_cmd_buffer = render_context.alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    sizeof(vuk::DrawIndexedIndirectCommand) * std::max(mesh_draw_count, 1_u32)
  );

  auto mesh_index_counts = stack.alloc<u32>(std::max(mesh_draw_count, 1_u32));
  mesh_index_counts[0] = 0;
  for (auto i = 0_u32; i < mesh_draw_count; i++) {
    mesh_index_counts[i] = context.mesh_draws[i].index_count;
  }
  auto mesh_index_counts_buffer = render_context.scratch_buffer_span(mesh_index_counts);

  auto build_draw_args_pass = vuk::make_pass(
    "particle build draw args",
    [mesh_draw_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) counters,
      VUK_BA(vuk::eComputeWrite) draw_cmd,
      VUK_BA(vuk::eComputeWrite) draw_indexed_cmd,
      VUK_BA(vuk::eComputeRead) index_counts
    ) {
      cmd_list //
        .bind_compute_pipeline("particle_build_draw_args")
        .bind_buffer(0, 0, counters)
        .bind_buffer(0, 1, draw_cmd)
        .bind_buffer(0, 2, draw_indexed_cmd)
        .bind_buffer(0, 3, index_counts)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mesh_draw_count))
        .dispatch(1);

      return std::make_tuple(counters, draw_cmd, draw_indexed_cmd, index_counts);
    }
  );

  std::tie(counters_buffer, draw_cmd_buffer, draw_indexed_cmd_buffer, mesh_index_counts_buffer) = build_draw_args_pass(
    std::move(counters_buffer),
    std::move(draw_cmd_buffer),
    std::move(draw_indexed_cmd_buffer),
    std::move(mesh_index_counts_buffer)
  );

  self.particle_pool_ping = !self.particle_pool_ping;

  context.particles_buffer = std::move(particles_buffer);
  context.sort_keys_buffer = std::move(sort_keys_buffer);
  context.draw_cmd_buffer = std::move(draw_cmd_buffer);
  context.draw_indexed_cmd_buffer = std::move(draw_indexed_cmd_buffer);
}

auto RendererInstance::draw_particles(
  this RendererInstance& self, ParticleContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;
  memory::ScopedStack stack;

  auto billboard_pass = vuk::make_pass(
    "particle billboards",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) draw_cmd,
      VUK_BA(vuk::eVertexRead) sort_keys,
      VUK_BA(vuk::eVertexRead) particles,
      VUK_BA(vuk::eVertexRead) emitters,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eFragmentRead) materials,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eColorRW) dst,
      VUK_IA(vuk::eColorRW) reactive
    ) {
      cmd_list //
        .bind_graphics_pipeline("particle_billboard")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .set_depth_stencil({})
        .set_color_blend(dst, PARTICLE_BLEND_STATE)
        .set_color_blend(reactive, REACTIVE_MASK_BLEND_STATE)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, sort_keys)
        .bind_buffer(0, 1, particles)
        .bind_buffer(0, 2, emitters)
        .bind_buffer(0, 3, camera)
        .bind_image(0, 4, depth)
        .bind_persistent(1, App::get_rendercontext().get_descriptor_set())
        .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(materials->device_address))
        .draw_indirect(1, draw_cmd);

      return std::make_tuple(sort_keys, particles, emitters, camera, materials, depth, dst, reactive);
    }
  );

  std::tie(
    context.sort_keys_buffer,
    context.particles_buffer,
    context.emitters_buffer,
    context.camera_buffer,
    context.materials_buffer,
    context.depth_attachment,
    dst_attachment,
    context.reactive_mask_attachment
  ) =
    billboard_pass(
      std::move(context.draw_cmd_buffer),
      std::move(context.sort_keys_buffer),
      std::move(context.particles_buffer),
      std::move(context.emitters_buffer),
      std::move(context.camera_buffer),
      std::move(context.materials_buffer),
      std::move(context.depth_attachment),
      std::move(dst_attachment),
      std::move(context.reactive_mask_attachment)
    );

  for (auto draw_index = 0_u32; draw_index < context.mesh_draws.size(); draw_index++) {
    const auto& mesh_draw = context.mesh_draws[draw_index];
    if (mesh_draw.index_count == 0) {
      continue;
    }

    auto mesh_buffer = self.renderer.render_context->scratch_buffer(mesh_draw.gpu_mesh);

    auto mesh_pass = vuk::make_pass(
      stack.format("particle mesh {}", mesh_draw.emitter_index),
      [emitter_index = mesh_draw.emitter_index, index_buffer = mesh_draw.index_buffer, draw_index](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eIndirectRead) draw_cmd,
        VUK_BA(vuk::eVertexRead) sort_keys,
        VUK_BA(vuk::eVertexRead) particles,
        VUK_BA(vuk::eVertexRead) emitters,
        VUK_BA(vuk::eVertexRead) camera,
        VUK_BA(vuk::eVertexRead) mesh,
        VUK_BA(vuk::eFragmentRead) materials,
        VUK_IA(vuk::eFragmentSampled) depth,
        VUK_IA(vuk::eColorRW) dst,
        VUK_IA(vuk::eColorRW) reactive
      ) {
        cmd_list //
          .bind_graphics_pipeline("particle_mesh")
          .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
          .set_depth_stencil({})
          .set_color_blend(dst, PARTICLE_BLEND_STATE)
          .set_color_blend(reactive, REACTIVE_MASK_BLEND_STATE)
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_buffer(0, 0, sort_keys)
          .bind_buffer(0, 1, particles)
          .bind_buffer(0, 2, emitters)
          .bind_buffer(0, 3, camera)
          .bind_image(0, 4, depth)
          .bind_persistent(1, App::get_rendercontext().get_descriptor_set())
          .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
          .push_constants(
            vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
            0,
            PushConstants(materials->device_address, mesh->device_address, emitter_index)
          )
          .draw_indexed_indirect(
            1,
            draw_cmd
              ->subrange(draw_index * sizeof(vuk::DrawIndexedIndirectCommand), sizeof(vuk::DrawIndexedIndirectCommand))
          );

        return std::make_tuple(draw_cmd, sort_keys, particles, emitters, camera, mesh, materials, depth, dst, reactive);
      }
    );

    auto mesh_value = vuk::Value<vuk::Buffer>{};
    std::tie(
      context.draw_indexed_cmd_buffer,
      context.sort_keys_buffer,
      context.particles_buffer,
      context.emitters_buffer,
      context.camera_buffer,
      mesh_value,
      context.materials_buffer,
      context.depth_attachment,
      dst_attachment,
      context.reactive_mask_attachment
    ) =
      mesh_pass(
        std::move(context.draw_indexed_cmd_buffer),
        std::move(context.sort_keys_buffer),
        std::move(context.particles_buffer),
        std::move(context.emitters_buffer),
        std::move(context.camera_buffer),
        std::move(mesh_buffer),
        std::move(context.materials_buffer),
        std::move(context.depth_attachment),
        std::move(dst_attachment),
        std::move(context.reactive_mask_attachment)
      );
  }

  return dst_attachment;
}
} // namespace ox
