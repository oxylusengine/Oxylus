#include "Render/RendererCVar.hpp"

namespace ox {

RendererCVar::RendererCVar() { init(); }

auto RendererCVar::init(this RendererCVar& self) -> void {
  ZoneScoped;

  self.cvar_enable_debug_renderer.init(self.system, "rr.debug_renderer", "enable debug renderer", 1);
  self.cvar_draw_bounding_boxes.init(self.system, "rr.draw_bounding_boxes", "draw mesh and sprite bounding boxes", 0);
  self.cvar_enable_physics_debug_renderer
    .init(self.system, "rr.physics_debug_renderer", "enable physics debug renderer", 0);
  self.cvar_freeze_culling_frustum.init(self.system, "rr.freeze_culling_frustum", "freeze culling frustum", 0);
  self.cvar_draw_camera_frustum.init(self.system, "rr.draw_camera_frustum", "draw camera frustum", 0);
  self.cvar_debug_view.init(
    self.system,
    "rr.debug_view",
    "0: None, 1: Triangles, 2: Meshlets, 3: Overdraw, 4: Materials, 5: Mesh Instances, 6: Mesh LoDs, 7: Albedo Color, "
    "8: Normal Color, 9: Emissive Color, 10: Metallic Color, 11: Roughness Color, 12: Baked Ambient Occlusion, 13: "
    "Screen Space Ambient Occlusion, 14: Geometric Normal, 15: Virtual Shadowmaps, 16: DDGI Probes",
    0
  );
  self.cvar_culling_frustum.init(self.system, "rr.culling_frustum", "Frustum Culling", 1);
  self.cvar_culling_occlusion.init(self.system, "rr.culling_occlusion", "Occlusion culling", 1);
  self.cvar_culling_triangle.init(self.system, "rr.culling_triangle", "Triangle culling", 1);
  self.cvar_transparent_background
    .init(self.system, "rr.transparent_background", "skip the sky and leave the background transparent", 0);

  self.cvar_contact_shadows_enabled.init(self.system, "pp.contact_shadows", "enable contact shadows", 1);
  self.cvar_contact_shadows_steps.init(self.system, "pp.contact_shadows_steps", "contact shadows steps", 8);
  self.cvar_contact_shadows_thickness
    .init(self.system, "pp.contact_shadows_thickness", "contact shadows thickness", 0.1f);
  self.cvar_contact_shadows_length.init(self.system, "pp.contact_shadows_thickness", "contact shadows length", 0.01f);

  self.cvar_vbgtao_enable.init(self.system, "pp.vbgtao", "use vbgtao", 1);
  self.cvar_vbgtao_quality_level
    .init(self.system, "pp.vbgtao_quality_level", "0: Low, 1: Medium, 2: High, 3: Ultra", 3);
  self.cvar_vbgtao_thickness.init(self.system, "pp.vbgtao_thickness", "vbgtao thickness", 0.25f);
  self.cvar_vbgtao_radius.init(self.system, "pp.vbgtao_radius", "vbgtao radius", 0.5f);
  self.cvar_vbgtao_final_power.init(self.system, "pp.vbgtao_final_power", "vbgtao final power", 1.2f);

  self.cvar_rtao_enable
    .init(self.system, "pp.rtao", "trace ambient occlusion against the scene TLAS instead of screen space", 0);
  self.cvar_rtao_ray_count.init(self.system, "pp.rtao_ray_count", "rays traced per pixel", 2);
  self.cvar_rtao_radius.init(self.system, "pp.rtao_radius", "rtao world space ray length", 1.0f);
  self.cvar_rtao_power.init(self.system, "pp.rtao_power", "rtao final power", 1.0f);

  self.cvar_ddgi_enable.init(self.system, "rr.ddgi", "enable dynamic diffuse global illumination probe volumes", 1);
  self.cvar_ddgi_rays_per_probe.init(self.system, "rr.ddgi_rays_per_probe", "rays traced per probe each frame", 128);
  self.cvar_ddgi_max_ray_distance
    .init(self.system, "rr.ddgi_max_ray_distance", "world space length of a probe ray", 50.0f);
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
  self.cvar_ddgi_probe_relocation
    .init(self.system, "rr.ddgi_probe_relocation", "move probes out of the geometry they are buried in", 1);
  self.cvar_ddgi_min_frontface_distance
    .init(self.system, "rr.ddgi_min_frontface_distance", "how far a probe keeps off a surface, in world units", 0.5f);
  self.cvar_ddgi_shadow_ray_offset
    .init(self.system, "rr.ddgi_shadow_ray_offset", "surface offset applied before tracing a shadow ray", 0.05f);
  self.cvar_ddgi_normal_bias
    .init(self.system, "rr.ddgi_normal_bias", "probe lookup offset along the normal, in probe spacings", 0.25f);
  self.cvar_ddgi_view_bias
    .init(self.system, "rr.ddgi_view_bias", "probe lookup offset toward the camera, in probe spacings", 0.1f);
  self.cvar_ddgi_intensity.init(self.system, "rr.ddgi_intensity", "scales the probe indirect diffuse", 1.0f);
  self.cvar_ddgi_hysteresis
    .init(self.system, "rr.ddgi_hysteresis", "how much of a probe's history survives each update", 0.97f);
  self.cvar_ddgi_probe_debug_radius
    .init(self.system, "rr.ddgi_probe_debug_radius", "world space radius of debug drawn probes", 0.1f);

  self.cvar_bloom_enable.init(self.system, "pp.bloom", "use bloom", 1);
  self.cvar_bloom_threshold.init(self.system, "pp.bloom_threshold", "bloom threshold", 1.0f);
  self.cvar_bloom_soft_threshold.init(self.system, "pp.bloom_soft_threshold", "bloom soft threshold", 0.125f);
  self.cvar_bloom_radius.init(self.system, "pp.bloom_radius", "bloom radius", 0.75f);
  self.cvar_bloom_intensity.init(self.system, "pp.bloom_intensity", "bloom intensity", 0.1f);
  self.cvar_bloom_clamp.init(self.system, "pp.bloom_clamp", "bloom source clamp", 4.0f);

  self.cvar_fxaa_enable.init(self.system, "pp.fxaa", "use fxaa", 1);

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

  writer["rtao"].begin_obj();
  writer["enabled"] = self.cvar_rtao_enable.as_bool();
  writer["ray_count"] = self.cvar_rtao_ray_count.get();
  writer["radius"] = self.cvar_rtao_radius.get();
  writer["power"] = self.cvar_rtao_power.get();
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
  writer["intensity"] = self.cvar_ddgi_intensity.get();
  writer["probe_debug_radius"] = self.cvar_ddgi_probe_debug_radius.get();
  writer.end_obj();

  writer["bloom"].begin_obj();
  writer["enabled"] = self.cvar_bloom_enable.as_bool();
  writer["threshold"] = self.cvar_bloom_threshold.get();
  writer["soft_threshold"] = self.cvar_bloom_soft_threshold.get();
  writer["radius"] = self.cvar_bloom_radius.get();
  writer["intensity"] = self.cvar_bloom_intensity.get();
  writer["clamp"] = self.cvar_bloom_clamp.get();
  writer.end_obj();

  writer["fxaa"].begin_obj();
  writer["enabled"] = self.cvar_fxaa_enable.as_bool();
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

  auto rtao_obj = json["rtao"];
  if (!rtao_obj.error()) {
    self.cvar_rtao_enable.set(rtao_obj["enabled"].get_bool());
    self.cvar_rtao_ray_count.set(static_cast<i32>(rtao_obj["ray_count"].get_int64()));
    self.cvar_rtao_radius.set(static_cast<f32>(rtao_obj["radius"].get_double()));
    self.cvar_rtao_power.set(static_cast<f32>(rtao_obj["power"].get_double()));
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
    self.cvar_ddgi_intensity.set(static_cast<f32>(ddgi_obj["intensity"].get_double()));
    self.cvar_ddgi_probe_debug_radius.set(static_cast<f32>(ddgi_obj["probe_debug_radius"].get_double()));
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

  auto fxaa_obj = json["fxaa"];
  if (!fxaa_obj.error()) {
    self.cvar_fxaa_enable.set(fxaa_obj["enabled"].get_bool());
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
