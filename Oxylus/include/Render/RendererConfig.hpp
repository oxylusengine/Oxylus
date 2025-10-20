#pragma once

#include <expected>

#include "Utils/CVars.hpp"

namespace ox {
namespace RendererCVar {
// clang-format off
inline AutoCVar_Int cvar_vsync("rr.vsync", "toggle vsync", 1);
inline AutoCVar_Int cvar_frame_limit("rr.frame_limit", "Limits the framerate with a sleep. 0: Disable, > 0: Enable", 0);

inline AutoCVar_Int cvar_enable_debug_renderer("rr.debug_renderer", "enable debug renderer", 1);
inline AutoCVar_Int cvar_draw_bounding_boxes("rr.draw_bounding_boxes", "draw mesh bounding boxes", 0);
inline AutoCVar_Int cvar_enable_physics_debug_renderer("rr.physics_debug_renderer", "enable physics debug renderer", 0);
inline AutoCVar_Int cvar_freeze_culling_frustum("rr.freeze_culling_frustum", "freeze culling frustum", 0);
inline AutoCVar_Int cvar_draw_camera_frustum("rr.draw_camera_frustum", "draw camera frustum", 0);
inline AutoCVar_Int cvar_debug_view("rr.debug_view", "0: None, 1: Triangles, 2: Meshlets, 3: Overdraw, 4: Materials, 5: Mesh Instances, 6: Mesh LoDs, 7: Albedo Color, 8: Normal Color, 9: Emissive Color, 10: Metallic Color, 11: Roughness Color, 12: Baked Ambient Occlusion, 13: Screen Space Ambient Occlusion", 0);
inline AutoCVar_Int cvar_culling_frustum("rr.culling_frustum", "Frustum Culling", 1);
inline AutoCVar_Int cvar_culling_occlusion("rr.culling_occlusion", "Occlusion culling", 1);
inline AutoCVar_Int cvar_culling_triangle("rr.culling_triangle", "Triangle culling", 1);

inline AutoCVar_Int cvar_reload_renderer("rr.reload_renderer", "reload renderer", 0);

inline AutoCVar_Int cvar_contact_shadows("pp.contact_shadows", "enable contact shadows", 1);
inline AutoCVar_Int cvar_contact_shadows_steps("pp.contact_shadows_steps", "contact shadows steps", 8);
inline AutoCVar_Float cvar_contact_shadows_thickness("pp.contact_shadows_thickness", "contact shadows thickness", 0.1);
inline AutoCVar_Float cvar_contact_shadows_length("pp.contact_shadows_thickness", "contact shadows length", 0.01);

inline AutoCVar_Int cvar_vbgtao_enable("pp.vbgtao", "use vbgtao", 1);
inline AutoCVar_Int cvar_vbgtao_quality_level("pp.vbgtao_quality_level", "0: Low, 1: Medium, 2: High, 3: Ultra", 3);
inline AutoCVar_Float cvar_vbgtao_thickness("pp.vbgtao_thickness", "vbgtao thickness", 0.25f);
inline AutoCVar_Float cvar_vbgtao_radius("pp.vbgtao_radius", "vbgtao radius", 0.5f);
inline AutoCVar_Float cvar_vbgtao_final_power("pp.vbgtao_final_power", "vbgtao final power", 1.2f);

inline AutoCVar_Int cvar_bloom_enable("pp.bloom", "use bloom", 1);
inline AutoCVar_Int cvar_bloom_quality_level("pp.bloom_quality_level", "0: Low, 1: Medium, 2: High, 3: Ultra", 3);
inline AutoCVar_Float cvar_bloom_threshold("pp.bloom_threshold", "bloom threshold", 2.5f);
inline AutoCVar_Float cvar_bloom_clamp("pp.bloom_clamp", "bloom clamp", 3);

inline AutoCVar_Int cvar_fxaa_enable("pp.fxaa", "use fxaa", 1);

inline AutoCVar_Int cvar_fsr_enable("pp.fsr", "use FSR", 1);
inline AutoCVar_Float cvar_fsr_sharpness("pp.fsr_sharpness", "sharpness for FSR", 0.5f);

inline AutoCVar_Int cvar_tonemapper("pp.tonemapper", "tonemapper preset", 0);
inline AutoCVar_Float cvar_exposure("pp.exposure", "tonemapping exposure", 1.0f);
inline AutoCVar_Float cvar_gamma("pp.gamma", "screen gamma", 2.2f);
// clang-format on
} // namespace RendererCVar

class RendererConfig {
public:
  constexpr static auto MODULE_NAME = "RendererConfig";

  enum Tonemaps {
    TONEMAP_DISABLED = 0,
    TONEMAP_ACES = 1,
    TONEMAP_UNCHARTED = 2,
    TONEMAP_FILMIC = 3,
    TONEMAP_REINHARD = 4,
  };

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  bool save_config(const char* path) const;
  bool load_config(const char* path);
};
} // namespace ox
