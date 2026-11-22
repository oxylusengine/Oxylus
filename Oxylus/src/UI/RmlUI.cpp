#include "UI/RmlUI.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <ranges>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Keycodes.hpp"
#include "Memory/Stack.hpp"
#include "UI/RmlView.hpp"
#include "Utils/Log.hpp"

namespace ox {
// mouse events carry no modifiers, so they are read when the event arrives: shift-click extends a text selection
static auto key_modifiers() -> i32 { return RmlSystem::convert_mod(static_cast<i16>(SDL_GetModState())); }

// Reverse order so the view registered last wins where two overlap.
static auto hit_test(std::span<RmlView* const> views, glm::vec2 position) -> RmlView* {
  for (auto* view : std::ranges::reverse_view(views)) {
    if (view->viewport_size.x <= 0.f || view->viewport_size.y <= 0.f) {
      continue;
    }

    const auto local = position - view->viewport_origin;
    if (local.x >= 0.f && local.y >= 0.f && local.x < view->viewport_size.x && local.y < view->viewport_size.y) {
      return view;
    }
  }

  return nullptr;
}

static auto to_surface_coords(const RmlView& view, glm::vec2 position) -> glm::ivec2 {
  const auto local = position - view.viewport_origin;

  return {
    static_cast<i32>(local.x * static_cast<f32>(view.surface_size.x) / view.viewport_size.x),
    static_cast<i32>(local.y * static_cast<f32>(view.surface_size.y) / view.viewport_size.y),
  };
}

auto rml_wants_keyboard(Rml::Context* context) -> bool {
  auto* focus = context ? context->GetFocusElement() : nullptr;
  if (!focus) {
    return false;
  }

  const auto& tag = focus->GetTagName();
  if (tag == "textarea") {
    return true;
  }

  if (tag != "input") {
    return false;
  }

  // RmlUi makes a text field of every input type it doesn't know
  const auto type = focus->GetAttribute<Rml::String>("type", "text");
  return type != "radio" && type != "checkbox" && type != "range" && type != "submit" && type != "button";
}

static auto normalized_path(std::string_view path) -> std::string {
  auto normalized = std::filesystem::path(path).lexically_normal().generic_string();
  // a document keeps a drive letter the way old urls spelled it, `C:/a` comes back as `C|/a`
  if (normalized.size() >= 2 && normalized[1] == '|' && std::isalpha(static_cast<unsigned char>(normalized[0]))) {
    normalized[1] = ':';
  }

  return normalized;
}

auto reload_rml_documents(Rml::Context* context, std::span<const std::string> paths) -> void {
  ZoneScoped;

  if (!context || paths.empty()) {
    return;
  }

  const auto is_style_changed = std::ranges::any_of(paths, [](std::string_view path) {
    return path.ends_with(".rcss");
  });
  const auto is_markup_changed = std::ranges::any_of(paths, [](std::string_view path) {
    return path.ends_with(".rml");
  });
  if (is_style_changed) {
    Rml::Factory::ClearStyleSheetCache();
  }

  // a template reaches a document only when the document is loaded again
  if (is_markup_changed) {
    Rml::Factory::ClearTemplateCache();
  }

  memory::ScopedStack stack;
  // loading and closing reorders the context's documents
  auto documents = stack.alloc<Rml::ElementDocument*>(static_cast<usize>(context->GetNumDocuments()));
  for (usize i = 0; i < documents.size(); i++) {
    documents[i] = context->GetDocument(static_cast<i32>(i));
  }

  for (auto* document : documents) {
    const auto url = document->GetSourceURL();
    // the debugger's documents are built in memory
    if (url.empty()) {
      continue;
    }

    const auto document_path = normalized_path(url);
    const auto is_edited = std::ranges::any_of(paths, [&document_path](std::string_view path) {
      return normalized_path(path) == document_path;
    });
    if (!is_edited) {
      if (is_style_changed) {
        document->ReloadStyleSheet();
      }

      continue;
    }

    const auto is_visible = document->IsVisible();
    const auto is_modal = document->IsModal();
    auto* replacement = context->LoadDocument(url);
    if (!replacement) {
      OX_LOG_ERROR("Couldn't reload {}, the document on screen stays as it was", url);
      continue;
    }

    document->Close();
    if (is_visible) {
      replacement->Show(is_modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
    }

    OX_LOG_INFO("Reloaded {}", url);
  }
}

auto RmlUI::init(this RmlUI& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::SetSystemInterface(&self.rml_system);
  // before `Initialise`, which is when RmlUi settles on one
  self.rml_files.vfs = &App::get_vfs();
  self.rml_files.asset_man = &App::mod<AssetManager>();
  Rml::SetFileInterface(&self.rml_files);

  // No `Rml::SetRenderInterface`: every context brings its own.
  if (!Rml::Initialise()) {
    return std::unexpected("Failed to initalize RmlUI!");
  }

  return {};
}

auto RmlUI::deinit(this RmlUI& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::Shutdown();

  return {};
}

auto RmlUI::register_view(this RmlUI& self, RmlView* view) -> void {
  ZoneScoped;

  OX_CHECK_NULL(view);

  self.views.push_back(view);

  if (!self.debugger_host_context && Rml::Debugger::Initialise(view->context())) {
    self.debugger_host_context = view->context();
  }
}

auto RmlUI::unregister_view(this RmlUI& self, RmlView* view) -> void {
  ZoneScoped;

  std::erase(self.views, view);

  if (self.hovered_view == view) {
    self.hovered_view = nullptr;
  }
  if (self.capture_view == view) {
    self.capture_view = nullptr;
    self.held_buttons = 0;
  }

  // The debugger draws through its host context, so it cannot outlive it.
  if (view->context() && self.debugger_host_context == view->context()) {
    Rml::Debugger::Shutdown();
    self.debugger_host_context = nullptr;
  }
}

auto RmlUI::keyboard_view(this RmlUI& self) -> RmlView* {
  for (auto* view : self.views) {
    if (view->keyboard_focused) {
      return view;
    }
  }

  return nullptr;
}

auto RmlUI::update_hover(this RmlUI& self, glm::vec2 position) -> RmlView* {
  auto* target = self.capture_view ? self.capture_view : hit_test(self.views, position);

  if (self.hovered_view && self.hovered_view != target) {
    // Without this the last hovered element stays lit once the cursor moves to another view.
    self.hovered_view->context()->ProcessMouseLeave();
  }
  self.hovered_view = target;

  if (target) {
    const auto coords = to_surface_coords(*target, position);
    target->context()->ProcessMouseMove(coords.x, coords.y, key_modifiers());
  }

  return target;
}

auto RmlUI::process_key(this RmlUI& self, u32 key_code, u16 mods, bool down) -> bool {
  ZoneScoped;

  auto* view = self.keyboard_view();
  if (!view) {
    return false;
  }

  auto* context = view->context();
  const auto key = RmlSystem::convert_key(key_code);
  const auto modifiers = RmlSystem::convert_mod(mods);
  const auto is_propagated = down ? context->ProcessKeyDown(key, modifiers) : context->ProcessKeyUp(key, modifiers);

  return !is_propagated || rml_wants_keyboard(context);
}

auto RmlUI::process_text(this RmlUI& self, std::string_view text) -> void {
  ZoneScoped;

  if (auto* view = self.keyboard_view()) {
    view->context()->ProcessTextInput(Rml::String(text));
  }
}

auto RmlUI::process_mouse_move(this RmlUI& self, glm::vec2 position) -> void {
  ZoneScoped;

  self.last_mouse_position = position;
  self.update_hover(position);
}

auto RmlUI::process_mouse_button(this RmlUI& self, u8 button, bool down) -> bool {
  ZoneScoped;

  i32 rml_button = 0;
  switch (static_cast<MouseCode>(button)) {
    case MouseCode::Left  : rml_button = 0; break;
    case MouseCode::Middle: rml_button = 2; break;
    case MouseCode::Right : rml_button = 1; break;
    default               : return false;
  }

  const auto button_bit = static_cast<u8>(1u << rml_button);

  // Re-resolve rather than trust the last motion event: the layout may have shifted under a
  // stationary cursor, and RmlUi applies the click wherever its own last move landed.
  auto* target = self.update_hover(self.last_mouse_position);

  if (down) {
    if (!target) {
      return false;
    }

    self.capture_view = target;
    self.held_buttons |= button_bit;

    auto* context = target->context();
    context->ProcessMouseButtonDown(rml_button, key_modifiers());
    const auto is_ui_press = context->IsMouseInteracting();
    if (is_ui_press) {
      self.ui_buttons |= button_bit;
    } else if (rml_wants_keyboard(context)) {
      // clicking into the game hands the keyboard back, RmlUi only moves focus to what was clicked
      context->GetFocusElement()->Blur();
    }

    return is_ui_press;
  }

  const auto is_ui_release = (self.ui_buttons & button_bit) != 0;
  self.ui_buttons &= static_cast<u8>(~button_bit);

  if (!target) {
    return is_ui_release;
  }

  target->context()->ProcessMouseButtonUp(rml_button, key_modifiers());

  self.held_buttons &= static_cast<u8>(~button_bit);
  if (self.held_buttons == 0) {
    self.capture_view = nullptr;
  }

  return is_ui_release;
}

auto RmlUI::process_mouse_scroll(this RmlUI& self, glm::vec2 offset) -> bool {
  ZoneScoped;

  auto* target = self.update_hover(self.last_mouse_position);
  if (!target) {
    return false;
  }

  const auto is_propagated = target->context()->ProcessMouseWheel(Rml::Vector2f(offset.x, -offset.y), key_modifiers());
  return !is_propagated || target->context()->IsMouseInteracting();
}

auto RmlUI::reload_ui_files(this RmlUI& self, std::span<const std::string> paths) -> void {
  ZoneScoped;

  for (auto* view : self.views) {
    reload_rml_documents(view->context(), paths);
  }
}
} // namespace ox
