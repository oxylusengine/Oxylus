#include "AboutPanel.hpp"

#include <SDL3/SDL_platform.h>
#include <algorithm>
#include <icons/IconsMaterialDesignIcons.h>
#include <string_view>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Core/Base.hpp"
#include "Core/Version.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "UI/UI.hpp"

namespace ox {
static auto draw_about_detail(std::string_view label, std::string_view value) -> void {
  ImGui::TableNextRow(ImGuiTableRowFlags_None, UI::scale(24.0f));
  ImGui::TableNextColumn();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(label.data(), label.data() + label.size());
  ImGui::PopStyleColor();
  ImGui::TableNextColumn();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(value.data(), value.data() + value.size());
  ImGui::PopTextWrapPos();
}

AboutPanel::AboutPanel() : EditorPanelState("About Oxylus", ICON_MDI_INFORMATION, false) {
  window_default_size = {560.0f, 0.0f};
  window_sizing_cond = ImGuiCond_Always;
  window_center_at_appear = true;
  window_center_cond = ImGuiCond_Appearing;
}

auto AboutPanel::on_render(this AboutPanel& self, const SwapchainInfo&) -> void {
  ZoneScoped;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UI::scale(ImVec2(24.0f, 20.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(12.0f, 8.0f)));
  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowSizeConstraints(
    {0.0f, 0.0f},
    {std::max(1.0f, viewport->WorkSize.x - UI::scale(32.0f)), std::max(1.0f, viewport->WorkSize.y - UI::scale(32.0f))}
  );

  constexpr auto window_flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  const auto contents_visible = self.on_begin(window_flags);
  OX_DEFER(&self) {
    self.on_end();
    ImGui::PopStyleVar(2);
  };
  if (!contents_visible) {
    return;
  }

  memory::ScopedStack stack;
  const auto& theme = App::mod<Editor>().editor_theme;
  const auto& build_info = engine_build_info();
  const auto& render_context = App::get_rendercontext();
  const std::string_view source_status = build_info.revision == "unknown" ? "Unavailable"
                                         : build_info.dirty               ? "Modified"
                                                                          : "Clean";
  const std::string_view platform = SDL_GetPlatform();
  const auto api_version = render_context.device_api_version;
  const auto vulkan_version = stack.format(
    "{}.{}.{}",
    VK_API_VERSION_MAJOR(api_version),
    VK_API_VERSION_MINOR(api_version),
    VK_API_VERSION_PATCH(api_version)
  );

  const auto content_x = ImGui::GetCursorPosX();
  const auto content_width = ImGui::GetContentRegionAvail().x;
  const auto banner_width = std::min(content_width, UI::scale(220.0f));
  theme.draw_banner(banner_width);
  ImGui::SetCursorPosX(content_x);

  const auto version_label = stack.format("Version {}", build_info.version);
  ImGui::PushFont(theme.bold_font, 22.0f);
  ImGui::SetCursorPosX(
    content_x + std::max(0.0f, (content_width - ImGui::CalcTextSize(version_label.data()).x) * 0.5f)
  );
  ImGui::TextUnformatted(version_label.data(), version_label.data() + version_label.size());
  ImGui::PopFont();

  constexpr std::string_view tagline = "A data-driven game engine";
  ImGui::SetCursorPosX(content_x + std::max(0.0f, (content_width - ImGui::CalcTextSize(tagline.data()).x) * 0.5f));
  ImGui::TextDisabled("%s", tagline.data());
  ImGui::Spacing();

  const auto details_visible = ImGui::BeginChild(
    "##BuildDetails",
    {},
    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders
  );
  if (details_visible) {
    ImGui::SeparatorText("Build");
    constexpr auto table_flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##Build", 2, table_flags)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, UI::scale(100.0f));
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
      draw_about_detail("Version", build_info.full_version);
      draw_about_detail("Revision", build_info.revision.substr(0, 12));
      UI::tooltip_hover(stack.null_terminate_cstr(build_info.revision));
      draw_about_detail("Build mode", build_info.build_mode);
      draw_about_detail("Source tree", source_status);
      ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("System");
    if (ImGui::BeginTable("##System", 2, table_flags)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, UI::scale(100.0f));
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
      draw_about_detail("Platform", platform);
      draw_about_detail("GPU", render_context.device_name);
      draw_about_detail("Vulkan", vulkan_version);
      UI::tooltip_hover("Vulkan API version supported by the selected GPU driver");
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();

  ImGui::Spacing();
  ImGui::TextDisabled("Free and open source. Apache License 2.0.");
  ImGui::Separator();

  const auto close_width = UI::scale(96.0f);
  const auto button_height = UI::scale(32.0f);
  const auto copy_width = std::max(
    1.0f,
    ImGui::GetContentRegionAvail().x - close_width - ImGui::GetStyle().ItemSpacing.x
  );
  constexpr auto copy_label = ICON_MDI_CONTENT_COPY "  Copy build info###CopyBuildInfo";
  constexpr auto copied_label = ICON_MDI_CHECK "  Copied###CopyBuildInfo";
  const auto copied = ImGui::GetTime() < self.copied_until;
  if (ImGui::Button(copied ? copied_label : copy_label, {copy_width, button_height})) {
    ImGui::SetClipboardText(stack.format_char(
      "Oxylus {} ({})\nRevision: {}\nSource tree: {}\nPlatform: {}\nGPU: {}\nVulkan: {}",
      build_info.full_version,
      build_info.build_mode,
      build_info.revision,
      source_status,
      platform,
      render_context.device_name,
      vulkan_version
    ));
    self.copied_until = ImGui::GetTime() + 2.0;
  }
  ImGui::SameLine();
  if (
    ImGui::Button("Close", {close_width, button_height}) ||
    (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
  ) {
    self.visible = false;
  }
}
} // namespace ox
