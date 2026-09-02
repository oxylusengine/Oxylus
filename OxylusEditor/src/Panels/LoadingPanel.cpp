#include "LoadingPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imspinner.h>
#include <system_error>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/ProjectPanel.hpp"
#include "Project/Project.hpp"
#include "UI/UI.hpp"

namespace ox {
static auto phase_spinner() -> void {
  ImSpinner::detail::SpinnerConfig config{};
  config.setSpinnerType(ImSpinner::e_st_ang);
  config.setSpeed(6.0f);
  config.setAngle(4.0f);
  config.setThickness(UI::scale(2.0f));
  config.setRadius(UI::scale(6.0f));
  config.setColor(ImColor(1.0f, 1.0f, 1.0f, 1.0f));
  ImSpinner::Spinner("##PhaseSpinner", config);
}

// One row of the checklist: a state glyph, the label, a bar, and the file the phase is chewing on.
static auto phase_row(
  const char* label,
  const bool active,
  const bool done,
  const usize completed,
  const usize total,
  const std::string_view current
) -> void {
  memory::ScopedStack stack;

  ImGui::PushID(label);
  const auto glyph_width = UI::scale(20.0f);

  const auto row_start = ImGui::GetCursorPos();
  if (active && !done) {
    ImGui::SetCursorPos({row_start.x + UI::scale(4.0f), row_start.y + UI::scale(4.0f)});
    phase_spinner();
    ImGui::SetCursorPos(row_start);
    ImGui::Dummy({glyph_width, ImGui::GetTextLineHeight()});
  } else {
    const auto color = done ? ImVec4(0.45f, 0.80f, 0.45f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(done ? ICON_MDI_CHECK : ICON_MDI_CIRCLE_SMALL);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy({glyph_width - ImGui::GetItemRectSize().x, 0.0f});
  }

  ImGui::SameLine();
  if (!active && !done) {
    ImGui::TextDisabled("%s", label);
  } else {
    ImGui::TextUnformatted(label);
  }

  if (total > 0) {
    ImGui::SameLine();
    UI::align_right(ImGui::CalcTextSize(stack.format_char("{} / {}", total, total)).x);
    ImGui::TextDisabled("%s", stack.format_char("{} / {}", completed, total));
  }

  const auto fraction = total > 0 ? static_cast<f32>(completed) / static_cast<f32>(total) : (done ? 1.0f : 0.0f);
  ImGui::SetCursorPosX(row_start.x + glyph_width);
  ImGui::ProgressBar(fraction, {-1.0f, UI::scale(5.0f)}, "");

  ImGui::SetCursorPosX(row_start.x + glyph_width);
  if (active && !current.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(current.data(), current.data() + current.size());
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled(" ");
  }

  ImGui::PopID();
}

LoadingPanel::LoadingPanel() : EditorPanelState("Loading", ICON_MDI_TIMER_SAND, false) {}

auto LoadingPanel::begin(this LoadingPanel& self, std::filesystem::path project_file_) -> void {
  self.project_file = std::move(project_file_);
  self.project_name = self.project_file.stem().string();
  self.phase = Phase::Discovering;
  self.failure.clear();
  self.close_requested = false;
  self.baking = {};
  self.registering = {};
  self.thumbnails = {};
  self.visible = true;
}

auto LoadingPanel::load_project(this LoadingPanel& self) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  const auto& active_project = editor.active_project;

  if (!active_project->load(self.project_file)) {
    OX_LOG_WARN("Couldn't load project: {}", self.project_file);
    self.failure = "The selected file is not a valid Oxylus project";
    self.phase = Phase::Failed;
    return;
  }

  auto& vfs = App::get_vfs();
  const auto start_scene = vfs.resolve_physical_dir(VFS::PROJECT_DIR, active_project->get_config().start_scene);
  editor.reset();
  // The project's assets are still cooking; `Editor::update` opens this once they are in.
  editor.pending_start_scene = start_scene;
  editor.reset_current_docking_layout();
  editor.editor_cvar.add_recent_project(active_project.get());
  editor.editor_panel_registry.get<ContentPanel>().init();

  self.project_name = active_project->get_config().name;
  self.phase = Phase::Cooking;
}

auto LoadingPanel::start_thumbnail_prewarm(this LoadingPanel& self) -> void {
  ZoneScoped;

  auto& vfs = App::get_vfs();
  if (!vfs.is_mounted_dir(VFS::PROJECT_DIR)) {
    return;
  }

  auto requests = std::vector<ThumbnailPrewarmRequest>();
  auto error = std::error_code();
  auto directory_it = std::filesystem::recursive_directory_iterator(
    vfs.resolve_physical_dir(VFS::PROJECT_DIR, ""),
    std::filesystem::directory_options::skip_permission_denied,
    error
  );

  const auto end = std::filesystem::recursive_directory_iterator();
  for (; !error && directory_it != end; directory_it.increment(error)) {
    const auto& path = directory_it->path();
    // dotfiles, and with them the editor's own caches when it runs from inside the asset directory
    if (path.filename().string().starts_with('.')) {
      if (directory_it->is_directory(error)) {
        directory_it.disable_recursion_pending();
      }
      continue;
    }

    if (directory_it->is_directory(error)) {
      continue;
    }

    auto kind = ThumbnailKind::Texture;
    switch (classify_file_type(path)) {
      case FileType::Texture : kind = ThumbnailKind::Texture; break;
      case FileType::Model   : kind = ThumbnailKind::Model; break;
      case FileType::Material: kind = ThumbnailKind::Material; break;
      case FileType::Terrain : kind = ThumbnailKind::Terrain; break;
      default                : continue;
    }

    requests.emplace_back(ThumbnailPrewarmRequest{.path = path, .kind = kind});
  }

  App::mod<Editor>().thumbnail_manager.begin_prewarm(std::move(requests));
}

auto LoadingPanel::on_update(this LoadingPanel& self) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();

  switch (self.phase) {
    case Phase::Discovering: {
      // A previous project's scan still holds `AssetDirectory*` slots, and `Project::load` would
      // spin the main thread waiting on it. Let it drain under the modal instead.
      if (asset_scan_active()) {
        break;
      }

      self.load_project();
      break;
    }
    case Phase::Cooking: {
      if (asset_scan_active()) {
        break;
      }

      self.phase = Phase::OpeningScene;
      break;
    }
    case Phase::OpeningScene: {
      // `Editor::update` opens it on the same frame the scan clears, ahead of this panel's tick, so
      // this is normally already satisfied on entry
      if (editor.pending_start_scene.has_value()) {
        break;
      }

      self.start_thumbnail_prewarm();
      self.phase = Phase::Thumbnails;
      break;
    }
    case Phase::Thumbnails: {
      const auto progress = editor.thumbnail_manager.prewarm_progress();
      if (progress.completed < progress.total) {
        break;
      }

      self.close_requested = true;
      break;
    }
    case Phase::Failed: break;
  }

  // Read once per frame so the bars keep their last value after a phase finishes rather than
  // collapsing back to zero when the scan retires.
  if (self.phase == Phase::Discovering || self.phase == Phase::Cooking) {
    const auto scan = asset_scan_progress();
    self.baking = {.completed = scan.compiled_completed, .total = scan.compiled_total, .current = scan.current_file};
    self.registering = {.completed = scan.registered_completed, .total = scan.registered_total};
  }

  if (self.phase == Phase::Thumbnails) {
    const auto progress = editor.thumbnail_manager.prewarm_progress();
    self.thumbnails = {.completed = progress.completed, .total = progress.total, .current = progress.current};
  }
}

auto LoadingPanel::on_render(this LoadingPanel& self, vuk::ImageAttachment) -> void {
  ZoneScoped;

  if (self.visible && !ImGui::IsPopupOpen("ProjectLoading"))
    ImGui::OpenPopup("ProjectLoading");

  constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBackground;

  UI::center_next_window();
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
  ImGui::SetNextWindowSize(UI::scale(ImVec2(520.0f, 0.0f)));
  if (ImGui::BeginPopupModal("ProjectLoading", nullptr, flags)) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    const auto contents_visible = ImGui::BeginChild(
      "##Contents",
      {},
      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding
    );
    ImGui::PopStyleColor();

    if (contents_visible) {
      UI::push_frame_style();

      if (self.phase == Phase::Failed) {
        ImGui::SeparatorText("Couldn't open the project");
        UI::spacing(2);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s  %s", ICON_MDI_ALERT_CIRCLE, self.failure.c_str());
        UI::spacing(2);
        if (ImGui::Button(ICON_MDI_ARROW_LEFT "  Back", {-1.0f, UI::scale(34.0f)})) {
          App::mod<Editor>().editor_panel_registry.get<ProjectPanel>().show_error(self.failure);
          self.close_requested = true;
        }
      } else {
        ImGui::SeparatorText(self.project_name.c_str());
        UI::spacing(2);

        const auto cooking = self.phase == Phase::Discovering || self.phase == Phase::Cooking;

        phase_row("Baking assets", cooking, !cooking, self.baking.completed, self.baking.total, self.baking.current);
        phase_row("Registering assets", cooking, !cooking, self.registering.completed, self.registering.total, {});
        phase_row("Opening scene", self.phase == Phase::OpeningScene, self.phase == Phase::Thumbnails, 0, 0, {});
        phase_row(
          "Generating thumbnails",
          self.phase == Phase::Thumbnails,
          false,
          self.thumbnails.completed,
          self.thumbnails.total,
          self.thumbnails.current
        );
      }

      UI::pop_frame_style();
    }
    ImGui::EndChild();

    // `EditorPanelRegistry` skips `on_render` once `visible` is false, so the popup has to be told
    // to close in the same frame or it stays up with nothing driving it.
    if (self.close_requested) {
      self.close_requested = false;
      self.visible = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  ImGui::PopStyleColor();
}
} // namespace ox
