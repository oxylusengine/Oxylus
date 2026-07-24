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
    "Screen Space Ambient Occlusion, 14: Virtual Shadowmaps",
    0
  );
  self.cvar_culling_frustum.init(self.system, "rr.culling_frustum", "Frustum Culling", 1);
  self.cvar_culling_occlusion.init(self.system, "rr.culling_occlusion", "Occlusion culling", 1);
  self.cvar_culling_triangle.init(self.system, "rr.culling_triangle", "Triangle culling", 1);

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

  self.cvar_bloom_enable.init(self.system, "pp.bloom", "use bloom", 1);
  self.cvar_bloom_threshold.init(self.system, "pp.bloom_threshold", "bloom threshold", 1.0f);
  self.cvar_bloom_soft_threshold.init(self.system, "pp.bloom_soft_threshold", "bloom soft threshold", 0.125f);
  self.cvar_bloom_radius.init(self.system, "pp.bloom_radius", "bloom radius", 0.75f);

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

  writer["bloom"].begin_obj();
  writer["enabled"] = self.cvar_bloom_enable.as_bool();
  writer["threshold"] = self.cvar_bloom_threshold.get();
  writer["soft_threshold"] = self.cvar_bloom_soft_threshold.get();
  writer["radius"] = self.cvar_bloom_radius.get();
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
    self.cvar_tonemapper.set(color_obj["tonemapper"].get_int64());
    self.cvar_exposure.set(color_obj["exposure"].get_double());
    self.cvar_gamma.set(color_obj["gamma"].get_double());
  }

  auto gtao_obj = json["gtao"];
  if (!gtao_obj.error()) {
    self.cvar_vbgtao_enable.set(gtao_obj["enabled"].get_bool());
    self.cvar_vbgtao_quality_level.set(gtao_obj["quality_level"].get_int64());
    self.cvar_vbgtao_thickness.set(gtao_obj["thickness"]->get_double());
    self.cvar_vbgtao_radius.set(gtao_obj["radius"].get_double());
    self.cvar_vbgtao_final_power.set(gtao_obj["final_power"].get_double());
  }

  auto bloom_obj = json["bloom"];
  if (!bloom_obj.error()) {
    self.cvar_bloom_enable.set(bloom_obj["enabled"].get_bool());
    self.cvar_bloom_threshold.set(bloom_obj["threshold"].get_double());
    self.cvar_bloom_soft_threshold.set(bloom_obj["soft_threshold"].get_double());
    self.cvar_bloom_radius.set(bloom_obj["radius"].get_double());
  }

  auto fxaa_obj = json["fxaa"];
  if (!fxaa_obj.error()) {
    self.cvar_fxaa_enable.set(fxaa_obj["enabled"].get_bool());
  }

  auto cs_obj = json["contact_shadows"];
  if (!cs_obj.error()) {
    self.cvar_contact_shadows_enabled.set(cs_obj["enabled"].get_bool());
    self.cvar_contact_shadows_steps.set(cs_obj["steps"].get_int64());
    self.cvar_contact_shadows_thickness.set(cs_obj["thickness"].get_double());
    self.cvar_contact_shadows_length.set(cs_obj["length"].get_double());
  }
}
} // namespace ox
