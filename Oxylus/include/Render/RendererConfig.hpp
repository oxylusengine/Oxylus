#pragma once

#include "Core/ESystem.hpp"
#include "Utils/CVars.hpp"

namespace ox {
namespace RendererCVar {
// clang-format off
inline AutoCVar_Int cvar_vsync("rr.vsync", "toggle vsync", 1);
inline AutoCVar_Int cvar_frame_limit("rr.frame_limit", "Limits the framerate with a sleep. 0: Disable, > 0: Enable", 0);

inline AutoCVar_Int cvar_draw_grid("rr.draw_grid", "draw editor scene grid", 1);
inline AutoCVar_Float cvar_draw_grid_distance("rr.grid_distance", "max grid distance", 20.f);

inline AutoCVar_Int cvar_enable_debug_renderer("rr.debug_renderer", "enable debug renderer", 1);
inline AutoCVar_Int cvar_draw_bounding_boxes("rr.draw_bounding_boxes", "draw mesh bounding boxes", 0);
inline AutoCVar_Int cvar_enable_physics_debug_renderer("rr.physics_debug_renderer", "enable physics debug renderer", 0);
inline AutoCVar_Int cvar_freeze_culling_frustum("rr.freeze_culling_frustum", "freeze culling frustum", 0);
inline AutoCVar_Int cvar_draw_camera_frustum("rr.draw_camera_frustum", "draw camera frustum", 0);
inline AutoCVar_Int cvar_debug_view("rr.debug_view", "0: None, 1: Triangles, 2: Meshlets, 3: Overdraw, 4: Albdeo, 5: Normal, 6: Emissive, 7: Metallic, 8: Roughness, 9: Occlusion, 10: HiZ", 0);
inline AutoCVar_Int cvar_culling_frustum("rr.culling_frustum", "Frustum Culling", 1);
inline AutoCVar_Int cvar_culling_occlusion("rr.culling_occlusion", "Occlusion culling", 1);
inline AutoCVar_Int cvar_culling_triangle("rr.culling_triangle", "Triangle culling", 1);

inline AutoCVar_Int cvar_reload_renderer("rr.reload_renderer", "reload renderer", 0);

inline AutoCVar_Int cvar_gtao_enable("pp.gtao", "use gtao", 1);
inline AutoCVar_Int cvar_gtao_quality_level("pp.gtao_quality_level", "gtao quality level", 1);
inline AutoCVar_Int cvar_gtao_denoise_passes("pp.gtao_denoise_passes", "amount of gtao denoise blur passes", 3);
inline AutoCVar_Float cvar_gtao_radius("pp.gtao_radius", "gtao radius", 0.5f);
inline AutoCVar_Float cvar_gtao_falloff_range("pp.gtao_falloff_range", "gtao falloff range", 0.615f);
inline AutoCVar_Float cvar_gtao_sample_distribution_power("pp.gtao_sample_distribution_power", "gtao sample distribution power", 2.0f);
inline AutoCVar_Float cvar_gtao_thin_occluder_compensation("pp.gtao_thin_occluder_compensation", "gtao thin occluder compensation", 0.0f);
inline AutoCVar_Float cvar_gtao_final_value_power("pp.gtao_final_value_power", "gtao final value power", 0.5f);
inline AutoCVar_Float cvar_gtao_depth_mip_sampling_offset("pp.gtao_depth_mip_sampling_offset", "gtao depth mip sampling offset", 3.30f);

inline AutoCVar_Int cvar_bloom_enable("pp.bloom", "use bloom", 1);
inline AutoCVar_Int cvar_bloom_mips("pp.bloom_mips", "amount of mips used in bloom pass", 8);
inline AutoCVar_Float cvar_bloom_threshold("pp.bloom_threshold", "bloom threshold", 2.5f);
inline AutoCVar_Float cvar_bloom_clamp("pp.bloom_clamp", "bloom clmap", 3);

inline AutoCVar_Int cvar_fxaa_enable("pp.fxaa", "use fxaa", 1);

inline AutoCVar_Int cvar_fsr_enable("pp.fsr", "use FSR", 1);
inline AutoCVar_Float cvar_fsr_sharpness("pp.fsr_sharpness", "sharpness for FSR", 0.5f);

inline AutoCVar_Int cvar_tonemapper("pp.tonemapper", "tonemapper preset", 0);
inline AutoCVar_Float cvar_exposure("pp.exposure", "tonemapping exposure", 1.0f);
inline AutoCVar_Float cvar_gamma("pp.gamma", "screen gamma", 2.2f);
// clang-format on
} // namespace RendererCVar

class RendererConfig : public ESystem {
public:
  enum Tonemaps {
    TONEMAP_DISABLED = 0,
    TONEMAP_ACES = 1,
    TONEMAP_UNCHARTED = 2,
    TONEMAP_FILMIC = 3,
    TONEMAP_REINHARD = 4,
  };

  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  bool save_config(const char* path) const;
  bool load_config(const char* path);
};
} // namespace ox
