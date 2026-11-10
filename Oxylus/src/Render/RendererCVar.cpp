#include "Render/RendererCVar.hpp"

#include "Scene/SceneGPU.hpp"

namespace ox {

RendererCVar::RendererCVar() { init(); }

auto RendererCVar::init(this RendererCVar& self) -> void {
  ZoneScoped;

  self.cvar_enable_debug_renderer.init(self.system, "rr.debug_renderer", "enable debug renderer", 1);
  self.cvar_draw_bounding_boxes.init(self.system, "rr.draw_bounding_boxes", "draw mesh and sprite bounding boxes", 0);
  self.cvar_enable_physics_debug_renderer
    .init(self.system, "rr.physics_debug_renderer", "enable physics debug renderer", 0);
  self.cvar_freeze_culling_frustum.init(self.system, "rr.freeze_culling_frustum", "freeze culling frustum", 0);
  self.cvar_draw_camera_frustum.init(self.system, "rr.draw_camera_frustum", "draw camera frustum", 1);
  self.cvar_debug_view.init(
    self.system,
    "rr.debug_view",
    "0: None, 1: Triangles, 2: Meshlets, 3: Overdraw, 4: Materials, 5: Mesh Instances, 6: Mesh LoDs, 7: Albedo Color, "
    "8: Normal Color, 9: Emissive Color, 10: Metallic Color, 11: Roughness Color, 12: Baked Ambient Occlusion, 13: "
    "Screen Space Ambient Occlusion, 14: Geometric Normal, 15: Virtual Shadowmaps, 16: Virtual Shadowmaps "
    "(Point/Spot), 17: DDGI Probes, 18: SSSR Reflection, 19: SSSR Confidence, 20: SSSR Variance, 21: SSSR Sample "
    "Count, 22: RTGI Half Resolution",
    0
  );
  self.cvar_culling_frustum.init(self.system, "rr.culling_frustum", "Frustum Culling", 1);
  self.cvar_culling_occlusion.init(self.system, "rr.culling_occlusion", "Occlusion culling", 1);
  self.cvar_culling_triangle.init(self.system, "rr.culling_triangle", "Triangle culling", 1);
  self.cvar_transparent_background
    .init(self.system, "rr.transparent_background", "skip the sky and leave the background transparent", 0);
  self.cvar_vsm_force_invalidate.init(
    self.system,
    "rr.vsm_force_invalidate",
    "reset every virtual shadowmap page each frame, so a still camera reproduces the worst-case rebuild",
    0
  );

  self.cvar_contact_shadows_enabled.init(self.system, "pp.contact_shadows", "enable contact shadows", 1);
  self.cvar_contact_shadows_steps.init(self.system, "pp.contact_shadows_steps", "contact shadows steps", 8);
  self.cvar_contact_shadows_thickness
    .init(self.system, "pp.contact_shadows_thickness", "contact shadows thickness", 0.1f);
  self.cvar_contact_shadows_length.init(self.system, "pp.contact_shadows_length", "contact shadows length", 0.01f);

  self.cvar_vbgtao_enable.init(self.system, "pp.vbgtao", "use vbgtao", 1);
  self.cvar_vbgtao_quality_level
    .init(self.system, "pp.vbgtao_quality_level", "0: Low, 1: Medium, 2: High, 3: Ultra", 3);
  self.cvar_vbgtao_thickness.init(self.system, "pp.vbgtao_thickness", "vbgtao thickness", 0.25f);
  self.cvar_vbgtao_radius.init(self.system, "pp.vbgtao_radius", "vbgtao radius", 0.5f);
  self.cvar_vbgtao_final_power.init(self.system, "pp.vbgtao_final_power", "vbgtao final power", 1.2f);

  self.cvar_rt_skinned_rebuild_budget.init(
    self.system,
    "rr.rt_skinned_rebuild_budget",
    "triangles a frame may spend rebuilding skinned acceleration structures, 0 for no cap",
    50000
  );
  self.cvar_rt_skinned_max_refits.init(
    self.system,
    "rr.rt_skinned_max_refits",
    "refits a skinned acceleration structure takes before its tree is rebuilt, subject to the budget",
    8
  );

  self.cvar_rtao_enable
    .init(self.system, "pp.rtao", "trace ambient occlusion against the scene TLAS instead of screen space", 0);
  self.cvar_rtao_ray_count.init(self.system, "pp.rtao_ray_count", "rays traced per pixel", 2);
  self.cvar_rtao_radius.init(self.system, "pp.rtao_radius", "rtao world space ray length", 1.0f);
  self.cvar_rtao_power.init(self.system, "pp.rtao_power", "rtao final power", 1.0f);

  self.cvar_sssr_enable.init(self.system, "rr.sssr", "enable stochastic screen space reflections", 0);
  self.cvar_sssr_roughness_threshold
    .init(self.system, "sssr.roughness_threshold", "GGX alpha above which pixels get no reflection ray", 0.2f);
  self.cvar_sssr_depth_thickness
    .init(self.system, "sssr.depth_thickness", "depth thickness used to validate hits", 0.015f);
  self.cvar_sssr_max_traversal_intersections
    .init(self.system, "sssr.max_traversal_intersections", "hierarchical traversal iteration cap", 128);
  self.cvar_sssr_min_traversal_occupancy
    .init(self.system, "sssr.min_traversal_occupancy", "wave occupancy below which rough rays stop traversing", 4);
  self.cvar_sssr_most_detailed_mip
    .init(self.system, "sssr.most_detailed_mip", "most detailed depth hierarchy mip the traversal starts from", 0);
  self.cvar_sssr_samples_per_quad
    .init(self.system, "sssr.samples_per_quad", "rays traced per 2x2 quad, one of 1, 2 or 4", 1);
  self.cvar_sssr_temporal_variance_guided
    .init(self.system, "sssr.temporal_variance_guided", "trace extra rays where last frame's variance is high", 1);
  self.cvar_sssr_variance_threshold
    .init(self.system, "sssr.variance_threshold", "variance above which a pixel gets an extra ray", 0.0f);
  self.cvar_sssr_temporal_stability
    .init(self.system, "sssr.temporal_stability", "how strongly the temporal pass favours history", 0.7f);
  self.cvar_rtgi_enable.init(self.system, "rr.rtgi", "ray traced diffuse global illumination", 0);
  self.cvar_rtgi_max_temporal_samples
    .init(self.system, "rr.rtgi_max_temporal_samples", "rtgi temporal history length in samples", 64);
  self.cvar_rtgi_fast_convergence_samples.init(
    self.system,
    "rr.rtgi_fast_convergence_samples",
    "rtgi history length below which a pixel still counts as converging",
    32.0f
  );
  self.cvar_rtgi_temporal_fast_history_frames
    .init(self.system, "rr.rtgi_temporal_fast_history_frames", "rtgi fast history window in frames", 4);
  self.cvar_rtgi_post_blur_max_width
    .init(self.system, "rr.rtgi_post_blur_max_width", "rtgi post blur maximum half width in half res pixels", 16);
  self.cvar_rtgi_ray_percentage
    .init(self.system, "rr.rtgi_ray_percentage", "rtgi hard ray budget in rays per half res pixel", 1.0f);
  self.cvar_rtgi_min_ray_budget
    .init(self.system, "rr.rtgi_min_ray_budget", "rtgi fraction of half res pixels that always get a base ray", 0.125f);
  self.cvar_rtgi_max_rays_per_pixel
    .init(self.system, "rr.rtgi_max_rays_per_pixel", "rtgi most rays one half res pixel may request per frame", 12);
  self.cvar_rtgi_firefly_filter
    .init(self.system, "rr.rtgi_firefly_filter", "clamp rtgi rays that are far brighter than their neighbourhood", 1);
  self.cvar_rtgi_firefly_tolerance
    .init(self.system, "rr.rtgi_firefly_tolerance", "rtgi firefly ceiling headroom above the neighbourhood mean", 4.0f);
  self.cvar_rtgi_pre_blur_width
    .init(self.system, "rr.rtgi_pre_blur_width", "rtgi pre blur radius in half res pixels", 20.0f);

  self.cvar_ddgi_enable.init(self.system, "rr.ddgi", "enable dynamic diffuse global illumination probe volumes", 1);
  self.cvar_ddgi_rays_per_probe
    .init(self.system, "rr.ddgi_rays_per_probe", "rays traced per probe each frame", GPU::DDGI_RAYS_PER_PROBE);
  self.cvar_ddgi_max_ray_distance.init(
    self.system,
    "rr.ddgi_max_ray_distance",
    "world space length of a cascade 0 probe ray, doubled per cascade",
    50.0f
  );
  self.cvar_ddgi_max_ray_radiance
    .init(self.system, "rr.ddgi_max_ray_radiance", "luminance cap on a single probe ray, tames fireflies", 25.0f);
  self.cvar_ddgi_update_max_interval
    .init(self.system, "rr.ddgi_update_max_interval", "most frames a probe may go without being retraced", 8);
  self.cvar_ddgi_update_full_rate_distance.init(
    self.system,
    "rr.ddgi_update_full_rate_distance",
    "probes within this distance of the camera retrace every frame",
    10.0f
  );
  self.cvar_ddgi_distance_culling.init(
    self.system,
    "rr.ddgi_distance_culling",
    "deactivate probes far from meshes and reuse cached probe radiance for far ray hits",
    1
  );
  self.cvar_ddgi_probe_relocation
    .init(self.system, "rr.ddgi_probe_relocation", "move probes out of the geometry they are buried in", 1);
  self.cvar_ddgi_min_frontface_distance
    .init(self.system, "rr.ddgi_min_frontface_distance", "how far a probe keeps off a surface, in world units", 0.5f);
  self.cvar_ddgi_shadow_ray_offset
    .init(self.system, "rr.ddgi_shadow_ray_offset", "surface bias used for DDGI shadow visibility", 0.05f);
  self.cvar_ddgi_normal_bias
    .init(self.system, "rr.ddgi_normal_bias", "probe lookup offset along the normal, in probe spacings", 0.25f);
  self.cvar_ddgi_view_bias
    .init(self.system, "rr.ddgi_view_bias", "probe lookup offset toward the camera, in probe spacings", 0.1f);
  self.cvar_ddgi_intensity.init(self.system, "rr.ddgi_intensity", "scales the probe indirect diffuse", 1.0f);
  self.cvar_ddgi_hysteresis
    .init(self.system, "rr.ddgi_hysteresis", "how much of a probe's history survives each update", 0.97f);
  self.cvar_ddgi_max_brightness_step.init(
    self.system,
    "rr.ddgi_max_brightness_step",
    "how far a probe may brighten in one update before the step is quartered, tames emissive flicker",
    0.1f
  );
  self.cvar_ddgi_firefly_ratio.init(
    self.system,
    "rr.ddgi_firefly_ratio",
    "how many times its own value a probe texel lets one ray carry before clamping it",
    32.0f
  );
  self.cvar_ddgi_hysteresis_dark_bias.init(
    self.system,
    "rr.ddgi_hysteresis_dark_bias",
    "encoded irradiance below which a texel stops treating a brightness swing as a lighting change",
    0.15f
  );
  self.cvar_ddgi_probe_debug_radius
    .init(self.system, "rr.ddgi_probe_debug_radius", "world space radius of debug drawn probes", 0.1f);
  self.cvar_ddgi_visibility_floor
    .init(self.system, "rr.ddgi_visibility_floor", "lowest weight the probe visibility test can give a probe", 0.0f);
  self.cvar_ddgi_visibility_power
    .init(self.system, "rr.ddgi_visibility_power", "exponent applied to the chebyshev probe visibility", 3.0f);
  self.cvar_ddgi_facing_bias
    .init(self.system, "rr.ddgi_facing_bias", "constant added to the probe facing weight", 0.05f);
  self.cvar_ddgi_conservative_distance.init(
    self.system,
    "rr.ddgi_conservative_distance",
    "probe visibility takes the nearest distance texel instead of filtering",
    1
  );
  self.cvar_ddgi_apply_terms
    .init(self.system, "rr.ddgi_apply_terms", "bit 0 applies the indirect diffuse, bit 1 the indirect specular", 3);
  self.cvar_ddgi_ambient_fill
    .init(self.system, "rr.ddgi_ambient_fill", "scales the ambient color blended in where probes lack coverage", 1.0f);

  self.cvar_bloom_enable.init(self.system, "pp.bloom", "use bloom", 1);
  self.cvar_bloom_threshold.init(self.system, "pp.bloom_threshold", "bloom threshold", 1.0f);
  self.cvar_bloom_soft_threshold.init(self.system, "pp.bloom_soft_threshold", "bloom soft threshold", 0.125f);
  self.cvar_bloom_radius.init(self.system, "pp.bloom_radius", "bloom radius", 0.75f);
  self.cvar_bloom_intensity.init(self.system, "pp.bloom_intensity", "bloom intensity", 0.1f);
  self.cvar_bloom_clamp.init(self.system, "pp.bloom_clamp", "bloom source clamp", 4.0f);
  self.cvar_particles_enable.init(self.system, "rr.particles", "simulate and draw GPU particle systems", 1);
  self.cvar_particle_sort.init(self.system, "rr.particle_sort", "sort particles back to front before drawing", 1);

  self.cvar_fxaa_enable.init(self.system, "pp.fxaa", "use fxaa", 0);

  self.cvar_upscaler_backend.init(self.system, "pp.upscaler_backend", "0: None, 1: FSR3", 1);
  self.cvar_upscaler_quality.init(
    self.system,
    "pp.upscaler_quality",
    "0: Native AA (1.0x), 1: Quality (1.5x), 2: Balanced (1.7x), 3: Performance (2.0x), 4: Ultra Performance (3.0x)",
    0
  );
  self.cvar_upscaler_sharpness
    .init(self.system, "pp.upscaler_sharpness", "RCAS sharpening strength, 0 skips the sharpen pass", 0.0f);
  self.cvar_upscaler_debug_view.init(
    self.system,
    "pp.upscaler_debug_view",
    "replace the upscaled output with an upscaler intermediate. 0: Off, 1: Dilated Motion Vectors, "
    "2: Disocclusion, 3: Reactive, 4: Shading Change, 5: Accumulation, 6: Luma Instability, 7: Dilated Depth",
    0
  );
  self.cvar_upscaler_disable_jitter
    .init(self.system, "pp.upscaler_disable_jitter", "hold the jitter at zero, to isolate jitter related artifacts", 0);

  self.cvar_tonemapper.init(self.system, "pp.tonemapper", "tonemapper preset", 0);
  self.cvar_exposure.init(self.system, "pp.exposure", "tonemapping exposure", 1.0f);
  self.cvar_gamma.init(self.system, "pp.gamma", "screen gamma", 2.2f);
}

auto RendererCVar::to_json(this const RendererCVar& self, JsonWriter& writer) -> void {
  ZoneScoped;

  writer["config"].begin_obj();

  writer["debug"].begin_obj();
  writer["enable_debug_renderer"] = self.cvar_enable_debug_renderer.as_bool();
  writer["draw_bounding_boxes"] = self.cvar_draw_bounding_boxes.as_bool();
  writer["enable_physics_debug_renderer"] = self.cvar_enable_physics_debug_renderer.as_bool();
  writer.end_obj();

  writer["color"].begin_obj();
  writer["tonemapper"] = self.cvar_tonemapper.get();
  writer["exposure"] = self.cvar_exposure.get();
  writer["gamma"] = self.cvar_gamma.get();
  writer.end_obj();

  writer["gtao"].begin_obj();
  writer["enabled"] = self.cvar_vbgtao_enable.as_bool();
  writer["quality_level"] = self.cvar_vbgtao_quality_level.get();
  writer["thickness"] = self.cvar_vbgtao_thickness.get();
  writer["radius"] = self.cvar_vbgtao_radius.get();
  writer["final_power"] = self.cvar_vbgtao_final_power.get();
  writer.end_obj();

  writer["raytracing"].begin_obj();
  writer["skinned_rebuild_budget"] = self.cvar_rt_skinned_rebuild_budget.get();
  writer["skinned_max_refits"] = self.cvar_rt_skinned_max_refits.get();
  writer.end_obj();

  writer["rtao"].begin_obj();
  writer["enabled"] = self.cvar_rtao_enable.as_bool();
  writer["ray_count"] = self.cvar_rtao_ray_count.get();
  writer["radius"] = self.cvar_rtao_radius.get();
  writer["power"] = self.cvar_rtao_power.get();
  writer.end_obj();

  writer["sssr"].begin_obj();
  writer["enabled"] = self.cvar_sssr_enable.as_bool();
  writer["roughness_threshold"] = self.cvar_sssr_roughness_threshold.get();
  writer["depth_thickness"] = self.cvar_sssr_depth_thickness.get();
  writer["max_traversal_intersections"] = self.cvar_sssr_max_traversal_intersections.get();
  writer["min_traversal_occupancy"] = self.cvar_sssr_min_traversal_occupancy.get();
  writer["most_detailed_mip"] = self.cvar_sssr_most_detailed_mip.get();
  writer["samples_per_quad"] = self.cvar_sssr_samples_per_quad.get();
  writer["temporal_variance_guided"] = self.cvar_sssr_temporal_variance_guided.as_bool();
  writer["variance_threshold"] = self.cvar_sssr_variance_threshold.get();
  writer["temporal_stability"] = self.cvar_sssr_temporal_stability.get();
  writer.end_obj();

  writer["rtgi"].begin_obj();
  writer["enabled"] = self.cvar_rtgi_enable.as_bool();
  writer["max_temporal_samples"] = self.cvar_rtgi_max_temporal_samples.get();
  writer["fast_convergence_samples"] = self.cvar_rtgi_fast_convergence_samples.get();
  writer["temporal_fast_history_frames"] = self.cvar_rtgi_temporal_fast_history_frames.get();
  writer["post_blur_max_width"] = self.cvar_rtgi_post_blur_max_width.get();
  writer["ray_percentage"] = self.cvar_rtgi_ray_percentage.get();
  writer["min_ray_budget"] = self.cvar_rtgi_min_ray_budget.get();
  writer["max_rays_per_pixel"] = self.cvar_rtgi_max_rays_per_pixel.get();
  writer["firefly_filter"] = self.cvar_rtgi_firefly_filter.as_bool();
  writer["firefly_tolerance"] = self.cvar_rtgi_firefly_tolerance.get();
  writer["pre_blur_width"] = self.cvar_rtgi_pre_blur_width.get();
  writer.end_obj();

  writer["ddgi"].begin_obj();
  writer["enabled"] = self.cvar_ddgi_enable.as_bool();
  writer["rays_per_probe"] = self.cvar_ddgi_rays_per_probe.get();
  writer["max_ray_distance"] = self.cvar_ddgi_max_ray_distance.get();
  writer["max_ray_radiance"] = self.cvar_ddgi_max_ray_radiance.get();
  writer["update_max_interval"] = self.cvar_ddgi_update_max_interval.get();
  writer["update_full_rate_distance"] = self.cvar_ddgi_update_full_rate_distance.get();
  writer["probe_relocation"] = self.cvar_ddgi_probe_relocation.as_bool();
  writer["min_frontface_distance"] = self.cvar_ddgi_min_frontface_distance.get();
  writer["shadow_ray_offset"] = self.cvar_ddgi_shadow_ray_offset.get();
  writer["normal_bias"] = self.cvar_ddgi_normal_bias.get();
  writer["view_bias"] = self.cvar_ddgi_view_bias.get();
  writer["hysteresis"] = self.cvar_ddgi_hysteresis.get();
  writer["max_brightness_step"] = self.cvar_ddgi_max_brightness_step.get();
  writer["firefly_ratio"] = self.cvar_ddgi_firefly_ratio.get();
  writer["hysteresis_dark_bias"] = self.cvar_ddgi_hysteresis_dark_bias.get();
  writer["intensity"] = self.cvar_ddgi_intensity.get();
  writer["probe_debug_radius"] = self.cvar_ddgi_probe_debug_radius.get();
  writer["distance_culling"] = self.cvar_ddgi_distance_culling.as_bool();
  writer["visibility_floor"] = self.cvar_ddgi_visibility_floor.get();
  writer["visibility_power"] = self.cvar_ddgi_visibility_power.get();
  writer["facing_bias"] = self.cvar_ddgi_facing_bias.get();
  writer["conservative_distance"] = self.cvar_ddgi_conservative_distance.as_bool();
  writer["ambient_fill"] = self.cvar_ddgi_ambient_fill.get();
  writer["apply_terms"] = self.cvar_ddgi_apply_terms.get();
  writer.end_obj();

  writer["bloom"].begin_obj();
  writer["enabled"] = self.cvar_bloom_enable.as_bool();
  writer["threshold"] = self.cvar_bloom_threshold.get();
  writer["soft_threshold"] = self.cvar_bloom_soft_threshold.get();
  writer["radius"] = self.cvar_bloom_radius.get();
  writer["intensity"] = self.cvar_bloom_intensity.get();
  writer["clamp"] = self.cvar_bloom_clamp.get();
  writer.end_obj();

  writer["particles"].begin_obj();
  writer["enabled"] = self.cvar_particles_enable.as_bool();
  writer["sort"] = self.cvar_particle_sort.as_bool();
  writer.end_obj();

  writer["fxaa"].begin_obj();
  writer["enabled"] = self.cvar_fxaa_enable.as_bool();
  writer.end_obj();

  writer["upscaler"].begin_obj();
  writer["backend"] = self.cvar_upscaler_backend.get();
  writer["quality"] = self.cvar_upscaler_quality.get();
  writer["sharpness"] = self.cvar_upscaler_sharpness.get();
  writer.end_obj();

  writer["contact_shadows"].begin_obj();
  writer["enabled"] = self.cvar_contact_shadows_enabled.as_bool();
  writer["steps"] = self.cvar_contact_shadows_steps.get();
  writer["thickness"] = self.cvar_contact_shadows_thickness.get();
  writer["length"] = self.cvar_contact_shadows_length.get();
  writer.end_obj();

  writer.end_obj(); // config obj
}

auto RendererCVar::from_json(this const RendererCVar& self, simdjson::ondemand::value& json) -> void {
  ZoneScoped;

  auto debug_obj = json["debug"];
  if (!debug_obj.error()) {
    self.cvar_enable_debug_renderer.set(debug_obj["enable_debug_renderer"].get_bool());
    self.cvar_draw_bounding_boxes.set(debug_obj["draw_bounding_boxes"].get_bool());
    self.cvar_enable_physics_debug_renderer.set(debug_obj["enable_physics_debug_renderer"].get_bool());
  }

  auto color_obj = json["color"];
  if (!color_obj.error()) {
    self.cvar_tonemapper.set(static_cast<i32>(color_obj["tonemapper"].get_int64()));
    self.cvar_exposure.set(static_cast<f32>(color_obj["exposure"].get_double()));
    self.cvar_gamma.set(static_cast<f32>(color_obj["gamma"].get_double()));
  }

  auto gtao_obj = json["gtao"];
  if (!gtao_obj.error()) {
    self.cvar_vbgtao_enable.set(gtao_obj["enabled"].get_bool());
    self.cvar_vbgtao_quality_level.set(static_cast<i32>(gtao_obj["quality_level"].get_int64()));
    self.cvar_vbgtao_thickness.set(static_cast<f32>(gtao_obj["thickness"]->get_double()));
    self.cvar_vbgtao_radius.set(static_cast<f32>(gtao_obj["radius"].get_double()));
    self.cvar_vbgtao_final_power.set(static_cast<f32>(gtao_obj["final_power"].get_double()));
  }

  auto raytracing_obj = json["raytracing"];
  if (!raytracing_obj.error()) {
    if (auto budget = raytracing_obj["skinned_rebuild_budget"]; !budget.error()) {
      self.cvar_rt_skinned_rebuild_budget.set(static_cast<i32>(budget.get_int64()));
    }
    if (auto max_refits = raytracing_obj["skinned_max_refits"]; !max_refits.error()) {
      self.cvar_rt_skinned_max_refits.set(static_cast<i32>(max_refits.get_int64()));
    }
  }

  auto rtao_obj = json["rtao"];
  if (!rtao_obj.error()) {
    self.cvar_rtao_enable.set(rtao_obj["enabled"].get_bool());
    self.cvar_rtao_ray_count.set(static_cast<i32>(rtao_obj["ray_count"].get_int64()));
    self.cvar_rtao_radius.set(static_cast<f32>(rtao_obj["radius"].get_double()));
    self.cvar_rtao_power.set(static_cast<f32>(rtao_obj["power"].get_double()));
  }

  auto sssr_obj = json["sssr"];
  if (!sssr_obj.error()) {
    self.cvar_sssr_enable.set(sssr_obj["enabled"].get_bool());
    self.cvar_sssr_roughness_threshold.set(static_cast<f32>(sssr_obj["roughness_threshold"].get_double()));
    self.cvar_sssr_depth_thickness.set(static_cast<f32>(sssr_obj["depth_thickness"].get_double()));
    self.cvar_sssr_max_traversal_intersections.set(
      static_cast<i32>(sssr_obj["max_traversal_intersections"].get_int64())
    );
    self.cvar_sssr_min_traversal_occupancy.set(static_cast<i32>(sssr_obj["min_traversal_occupancy"].get_int64()));
    self.cvar_sssr_most_detailed_mip.set(static_cast<i32>(sssr_obj["most_detailed_mip"].get_int64()));
    self.cvar_sssr_samples_per_quad.set(static_cast<i32>(sssr_obj["samples_per_quad"].get_int64()));
    self.cvar_sssr_temporal_variance_guided.set(sssr_obj["temporal_variance_guided"].get_bool());
    self.cvar_sssr_variance_threshold.set(static_cast<f32>(sssr_obj["variance_threshold"].get_double()));
    self.cvar_sssr_temporal_stability.set(static_cast<f32>(sssr_obj["temporal_stability"].get_double()));
  }

  auto rtgi_obj = json["rtgi"];
  if (!rtgi_obj.error()) {
    self.cvar_rtgi_enable.set(rtgi_obj["enabled"].get_bool());
    if (auto samples = rtgi_obj["max_temporal_samples"]; !samples.error()) {
      self.cvar_rtgi_max_temporal_samples.set(static_cast<i32>(samples.get_int64()));
    }
    if (auto convergence = rtgi_obj["fast_convergence_samples"]; !convergence.error()) {
      self.cvar_rtgi_fast_convergence_samples.set(static_cast<f32>(convergence.get_double()));
    }
    if (auto frames = rtgi_obj["temporal_fast_history_frames"]; !frames.error()) {
      self.cvar_rtgi_temporal_fast_history_frames.set(static_cast<i32>(frames.get_int64()));
    }
    if (auto width = rtgi_obj["post_blur_max_width"]; !width.error()) {
      self.cvar_rtgi_post_blur_max_width.set(static_cast<i32>(width.get_int64()));
    }
    if (auto percentage = rtgi_obj["ray_percentage"]; !percentage.error()) {
      self.cvar_rtgi_ray_percentage.set(static_cast<f32>(percentage.get_double()));
    }
    if (auto budget = rtgi_obj["min_ray_budget"]; !budget.error()) {
      self.cvar_rtgi_min_ray_budget.set(static_cast<f32>(budget.get_double()));
    }
    if (auto rays = rtgi_obj["max_rays_per_pixel"]; !rays.error()) {
      self.cvar_rtgi_max_rays_per_pixel.set(static_cast<i32>(rays.get_int64()));
    }
    if (auto filter = rtgi_obj["firefly_filter"]; !filter.error()) {
      self.cvar_rtgi_firefly_filter.set(filter.get_bool());
    }
    if (auto tolerance = rtgi_obj["firefly_tolerance"]; !tolerance.error()) {
      self.cvar_rtgi_firefly_tolerance.set(static_cast<f32>(tolerance.get_double()));
    }
    if (auto width = rtgi_obj["pre_blur_width"]; !width.error()) {
      self.cvar_rtgi_pre_blur_width.set(static_cast<f32>(width.get_double()));
    }
  }

  auto ddgi_obj = json["ddgi"];
  if (!ddgi_obj.error()) {
    self.cvar_ddgi_enable.set(ddgi_obj["enabled"].get_bool());
    self.cvar_ddgi_rays_per_probe.set(static_cast<i32>(ddgi_obj["rays_per_probe"].get_int64()));
    self.cvar_ddgi_max_ray_distance.set(static_cast<f32>(ddgi_obj["max_ray_distance"].get_double()));
    self.cvar_ddgi_max_ray_radiance.set(static_cast<f32>(ddgi_obj["max_ray_radiance"].get_double()));
    self.cvar_ddgi_update_max_interval.set(static_cast<i32>(ddgi_obj["update_max_interval"].get_int64()));
    self.cvar_ddgi_update_full_rate_distance.set(static_cast<f32>(ddgi_obj["update_full_rate_distance"].get_double()));
    self.cvar_ddgi_probe_relocation.set(ddgi_obj["probe_relocation"].get_bool());
    self.cvar_ddgi_min_frontface_distance.set(static_cast<f32>(ddgi_obj["min_frontface_distance"].get_double()));
    self.cvar_ddgi_shadow_ray_offset.set(static_cast<f32>(ddgi_obj["shadow_ray_offset"].get_double()));
    self.cvar_ddgi_normal_bias.set(static_cast<f32>(ddgi_obj["normal_bias"].get_double()));
    self.cvar_ddgi_view_bias.set(static_cast<f32>(ddgi_obj["view_bias"].get_double()));
    self.cvar_ddgi_hysteresis.set(static_cast<f32>(ddgi_obj["hysteresis"].get_double()));
    self.cvar_ddgi_max_brightness_step.set(static_cast<f32>(ddgi_obj["max_brightness_step"].get_double()));
    self.cvar_ddgi_firefly_ratio.set(static_cast<f32>(ddgi_obj["firefly_ratio"].get_double()));
    self.cvar_ddgi_hysteresis_dark_bias.set(static_cast<f32>(ddgi_obj["hysteresis_dark_bias"].get_double()));
    self.cvar_ddgi_intensity.set(static_cast<f32>(ddgi_obj["intensity"].get_double()));
    self.cvar_ddgi_probe_debug_radius.set(static_cast<f32>(ddgi_obj["probe_debug_radius"].get_double()));
    auto distance_culling_obj = ddgi_obj["distance_culling"];
    if (!distance_culling_obj.error())
      self.cvar_ddgi_distance_culling.set(distance_culling_obj.get_bool());
    if (auto floor = ddgi_obj["visibility_floor"]; !floor.error()) {
      self.cvar_ddgi_visibility_floor.set(static_cast<f32>(floor.get_double()));
    }
    if (auto power = ddgi_obj["visibility_power"]; !power.error()) {
      self.cvar_ddgi_visibility_power.set(static_cast<f32>(power.get_double()));
    }
    if (auto bias = ddgi_obj["facing_bias"]; !bias.error()) {
      self.cvar_ddgi_facing_bias.set(static_cast<f32>(bias.get_double()));
    }
    if (auto conservative = ddgi_obj["conservative_distance"]; !conservative.error()) {
      self.cvar_ddgi_conservative_distance.set(conservative.get_bool());
    }
    if (auto terms = ddgi_obj["apply_terms"]; !terms.error()) {
      self.cvar_ddgi_apply_terms.set(static_cast<i32>(terms.get_int64()));
    }
    if (auto fill = ddgi_obj["ambient_fill"]; !fill.error()) {
      self.cvar_ddgi_ambient_fill.set(static_cast<f32>(fill.get_double()));
    }
  }

  auto bloom_obj = json["bloom"];
  if (!bloom_obj.error()) {
    self.cvar_bloom_threshold.set(static_cast<f32>(bloom_obj["threshold"].get_double()));
    self.cvar_bloom_soft_threshold.set(static_cast<f32>(bloom_obj["soft_threshold"].get_double()));
    self.cvar_bloom_radius.set(static_cast<f32>(bloom_obj["radius"].get_double()));
    auto intensity_obj = bloom_obj["intensity"];
    if (!intensity_obj.error())
      self.cvar_bloom_intensity.set(static_cast<f32>(intensity_obj.get_double()));
    auto clamp_obj = bloom_obj["clamp"];
    if (!clamp_obj.error())
      self.cvar_bloom_clamp.set(static_cast<f32>(clamp_obj.get_double()));
  }

  auto particles_obj = json["particles"];
  if (!particles_obj.error()) {
    self.cvar_particles_enable.set(particles_obj["enabled"].get_bool());
    self.cvar_particle_sort.set(particles_obj["sort"].get_bool());
  }

  auto fxaa_obj = json["fxaa"];
  if (!fxaa_obj.error()) {
    self.cvar_fxaa_enable.set(fxaa_obj["enabled"].get_bool());
  }

  auto upscaler_obj = json["upscaler"];
  if (!upscaler_obj.error()) {
    self.cvar_upscaler_backend.set(static_cast<i32>(upscaler_obj["backend"].get_int64()));
    self.cvar_upscaler_quality.set(static_cast<i32>(upscaler_obj["quality"].get_int64()));
    self.cvar_upscaler_sharpness.set(static_cast<f32>(upscaler_obj["sharpness"].get_double()));
  }

  auto cs_obj = json["contact_shadows"];
  if (!cs_obj.error()) {
    self.cvar_contact_shadows_enabled.set(cs_obj["enabled"].get_bool());
    self.cvar_contact_shadows_steps.set(static_cast<i32>(cs_obj["steps"].get_int64()));
    self.cvar_contact_shadows_thickness.set(static_cast<f32>(cs_obj["thickness"].get_double()));
    self.cvar_contact_shadows_length.set(static_cast<f32>(cs_obj["length"].get_double()));
  }
}
} // namespace ox
