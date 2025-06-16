#include "RendererSettingsPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>

#include "Core/App.hpp"
#include "EditorUI.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Vulkan/VkContext.hpp"

namespace ox {
RendererSettingsPanel::RendererSettingsPanel() : EditorPanel("Renderer Settings", ICON_MDI_GPU, true) {}

void RendererSettingsPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  if (on_begin()) {
    float avg = 0.0;

    const size_t size = m_FrameTimes.size();
    if (size >= 50)
      m_FrameTimes.erase(m_FrameTimes.begin());

    m_FrameTimes.emplace_back(ImGui::GetIO().Framerate);
    for (uint32_t i = 0; i < size; i++) {
      const float frame_time = m_FrameTimes[i];
      m_FpsValues[i] = frame_time;
      avg += frame_time;
    }
    avg /= (float)size;
    const double fps = 1.0 / static_cast<double>(avg) * 1000.0;
    ImGui::Text("FPS: %lf / (ms): %lf", static_cast<double>(avg), fps);
    ImGui::Text("GPU: %s", App::get_vkcontext().device_name.c_str());
    UI::tooltip_hover("Current viewport resolution");

    ImGui::Separator();
    if (UI::icon_button(ICON_MDI_RELOAD, "Reload render pipeline"))
      RendererCVar::cvar_reload_render_pipeline.toggle();
    ImGui::SeparatorText("Debug");
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("Draw AABBs", (bool*)RendererCVar::cvar_draw_bounding_boxes.get_ptr());
      UI::property("Draw meshlet AABBs", (bool*)RendererCVar::cvar_draw_meshlet_aabbs.get_ptr());
      UI::property("Physics renderer", (bool*)RendererCVar::cvar_enable_physics_debug_renderer.get_ptr());
      UI::end_properties();
    }

    ImGui::SeparatorText("Environment");
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      const char* tonemaps[5] = {"Disabled", "ACES", "Uncharted2", "Filmic", "Reinhard"};
      UI::property("Tonemapper", RendererCVar::cvar_tonemapper.get_ptr(), tonemaps, 5);
      UI::property<float>("Exposure", RendererCVar::cvar_exposure.get_ptr(), 0, 5, "%.2f");
      UI::property<float>("Gamma", RendererCVar::cvar_gamma.get_ptr(), 0, 5, "%.2f");
      UI::end_properties();
    }

    ImGui::SeparatorText("GTAO");
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("Enabled", (bool*)RendererCVar::cvar_gtao_enable.get_ptr());
      UI::property<int>("Denoise Passes", RendererCVar::cvar_gtao_denoise_passes.get_ptr(), 1, 5);
      UI::property<float>("Radius", RendererCVar::cvar_gtao_radius.get_ptr(), 0, 1);
      UI::property<float>("Falloff Range", RendererCVar::cvar_gtao_falloff_range.get_ptr(), 0, 1);
      UI::property<float>(
          "Sample Distribution Power", RendererCVar::cvar_gtao_sample_distribution_power.get_ptr(), 0, 5);
      UI::property<float>(
          "Thin Occluder Compensation", RendererCVar::cvar_gtao_thin_occluder_compensation.get_ptr(), 0, 5);
      UI::property<float>("Final Value Power", RendererCVar::cvar_gtao_final_value_power.get_ptr(), 0, 5);
      UI::property<float>(
          "Depth Mip Sampling Offset", RendererCVar::cvar_gtao_depth_mip_sampling_offset.get_ptr(), 0, 5);
      UI::end_properties();
    }

    ImGui::SeparatorText("Bloom");
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("Enabled", (bool*)RendererCVar::cvar_bloom_enable.get_ptr());
      UI::property<float>("Threshold", RendererCVar::cvar_bloom_threshold.get_ptr(), 0, 5);
      UI::property<float>("Clamp", RendererCVar::cvar_bloom_clamp.get_ptr(), 0, 5);
      UI::end_properties();
    }

    ImGui::SeparatorText("SSR");
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("Enabled", (bool*)RendererCVar::cvar_ssr_enable.get_ptr());
      UI::property("Samples", RendererCVar::cvar_ssr_samples.get_ptr(), 30, 1024);
      UI::property("Max Distance", RendererCVar::cvar_ssr_max_dist.get_ptr(), 50.0f, 500.0f);
      UI::end_properties();
    }

    ImGui::SeparatorText("FXAA");
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("Enabled", (bool*)RendererCVar::cvar_fxaa_enable.get_ptr());
      UI::end_properties();
    }
  }
  on_end();
}
} // namespace ox
