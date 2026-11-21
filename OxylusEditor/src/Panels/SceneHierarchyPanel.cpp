#include "SceneHierarchyPanel.hpp"

#include <algorithm>
#include <array>
#include <glm/geometric.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <span>
#include <string_view>
#include <utility>

#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Scene/ComponentRegistry.hpp"
#ifdef OX_DOTNET
  #include "Scripting/DotNetScripting.hpp"
#endif
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/ImGuiScoped.hpp"

namespace ox {
static constexpr auto COMPONENT_SEARCH_PREFIX = std::string_view("component:");

struct ComponentSearchAlias {
  const c8* name;
  const c8* description;
  bool (*matches)(flecs::entity);
};

template <auto Member, auto Value>
static auto matches_component_value(flecs::entity entity) -> bool {
  const auto* component = entity.try_get<refl::member_owner_t<Member>>();
  return component && (component->*Member) == Value;
}

static constexpr auto COMPONENT_SEARCH_ALIASES = std::to_array<ComponentSearchAlias>({
  {"DirectionalLightComponent",
   "Lights with type Directional",
   matches_component_value<&LightComponent::type, LightComponent::Directional>},
  {"SpotLightComponent", "Lights with type Spot", matches_component_value<&LightComponent::type, LightComponent::Spot>},
  {"PointLightComponent",
   "Lights with type Point",
   matches_component_value<&LightComponent::type, LightComponent::Point>},
  {"PerspectiveCameraComponent",
   "Cameras with perspective projection",
   matches_component_value<&CameraComponent::projection, CameraComponent::Perspective>},
  {"OrthographicCameraComponent",
   "Cameras with orthographic projection",
   matches_component_value<&CameraComponent::projection, CameraComponent::Orthographic>},
  {"StaticRigidBodyComponent",
   "Rigid bodies with type Static",
   matches_component_value<&RigidBodyComponent::type, RigidBodyComponent::Static>},
  {"KinematicRigidBodyComponent",
   "Rigid bodies with type Kinematic",
   matches_component_value<&RigidBodyComponent::type, RigidBodyComponent::Kinematic>},
  {"DynamicRigidBodyComponent",
   "Rigid bodies with type Dynamic",
   matches_component_value<&RigidBodyComponent::type, RigidBodyComponent::Dynamic>},
  {"SensorRigidBodyComponent",
   "Rigid bodies with Is Sensor enabled",
   matches_component_value<&RigidBodyComponent::is_sensor, true>},
  {"ConvexMeshColliderComponent",
   "Mesh colliders configured to use a convex hull",
   matches_component_value<&MeshColliderComponent::convex, true>},
  {"TriangleMeshColliderComponent",
   "Mesh colliders configured to use triangles",
   matches_component_value<&MeshColliderComponent::convex, false>},
  {"AudioSource3DComponent",
   "Audio sources with spatialization enabled",
   matches_component_value<&AudioSourceComponent::spatialization, true>},
  {"AudioSource2DComponent",
   "Audio sources with spatialization disabled",
   matches_component_value<&AudioSourceComponent::spatialization, false>},
  {"FrontWheelDriveVehicleComponent",
   "Vehicles with front-wheel drive",
   matches_component_value<&VehicleComponent::drive_mode, VehicleComponent::FrontWheelDrive>},
  {"RearWheelDriveVehicleComponent",
   "Vehicles with rear-wheel drive",
   matches_component_value<&VehicleComponent::drive_mode, VehicleComponent::RearWheelDrive>},
  {"AllWheelDriveVehicleComponent",
   "Vehicles with all-wheel drive",
   matches_component_value<&VehicleComponent::drive_mode, VehicleComponent::AllWheelDrive>},
});

static auto draw_component_search_help(f32 button_size) -> void {
  UI::button(ICON_MDI_HELP_CIRCLE_OUTLINE "###HierarchySearchHelp", {button_size, button_size});
  if (!ImGui::BeginItemTooltip()) {
    return;
  }

  ImGui::TextUnformatted("Plain text matches entity names. Component queries accept partial names and ignore case.");
  ImGui::TextUnformatted("Aliases match a setting on an existing component.");
  ImGui::TextUnformatted("Combine filters with spaces. Prefix a filter with - to exclude its matches.");
  ImGui::TextUnformatted("Use quotes for an exact component name. Use Up/Down and Tab to complete component names.");

  constexpr auto table_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  if (ImGui::BeginTable("ComponentSearchExamples", 2, table_flags)) {
    ImGui::TableSetupColumn("Query");
    ImGui::TableSetupColumn("Matches");
    ImGui::TableHeadersRow();

    const auto example = [](const c8* name, const c8* description) -> void {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(description);
    };
    example("component:CameraComponent", "Any camera");
    example("component:LightComponent", "Any light type");
    example("component:Collider", "Any collider component");
    example("component:LightComponent state:disabled", "Disabled lights");
    example("component:MeshComponent -component:Collider", "Meshes without a collider");
    example("name:\"Main Camera\"", "Entity names containing Main Camera");
    memory::ScopedStack stack;
    for (const auto& alias : COMPONENT_SEARCH_ALIASES) {
      example(stack.format_char("{}{}", COMPONENT_SEARCH_PREFIX, alias.name), alias.description);
    }
    ImGui::EndTable();
  }
  ImGui::EndTooltip();
}

enum class SearchField : u8 { Text, Name, Component, State };

struct SearchWord {
  usize begin;
  usize end;
};

struct SearchTerm {
  SearchField field = SearchField::Text;
  std::string_view value;
  bool exclude = false;
  bool quoted = false;
};

static constexpr auto SEARCH_WHITESPACE = std::string_view(" \t");

static auto next_search_word(std::string_view query, usize offset) -> option<SearchWord> {
  const auto begin = query.find_first_not_of(SEARCH_WHITESPACE, offset);
  if (begin == std::string_view::npos) {
    return nullopt;
  }

  auto end = begin;
  bool in_quotes = false;
  for (; end < query.size(); ++end) {
    if (query[end] == '"') {
      in_quotes = !in_quotes;
    } else if (!in_quotes && SEARCH_WHITESPACE.contains(query[end])) {
      break;
    }
  }
  return SearchWord{begin, end};
}

static auto starts_with_query_prefix(std::string_view value, std::string_view prefix) -> bool {
  return value.size() >= prefix.size() && ImStrnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

static auto contains_search_text(const c8* name, std::string_view value) -> bool {
  return name && ImStristr(name, nullptr, value.data(), value.data() + value.size()) != nullptr;
}

static auto parse_search_term(std::string_view word) -> SearchTerm {
  auto term = SearchTerm{};
  if (word.starts_with('-')) {
    term.exclude = true;
    word.remove_prefix(1);
  }

  constexpr auto fields = std::array{
    std::pair{SearchField::Component, COMPONENT_SEARCH_PREFIX},
    std::pair{SearchField::State, std::string_view("state:")},
    std::pair{SearchField::Name, std::string_view("name:")},
  };
  for (const auto& [kind, prefix] : fields) {
    if (starts_with_query_prefix(word, prefix)) {
      term.field = kind;
      word.remove_prefix(prefix.size());
      break;
    }
  }

  if (word.starts_with('"')) {
    term.quoted = true;
    word.remove_prefix(1);
    if (word.ends_with('"')) {
      word.remove_suffix(1);
    }
  }
  term.value = word;
  return term;
}

struct ParsedSearchTerm {
  SearchWord word;
  SearchTerm term;
};

static auto next_search_term(std::string_view query, usize offset) -> option<ParsedSearchTerm> {
  const auto word = next_search_word(query, offset);
  if (!word) {
    return nullopt;
  }

  auto parsed = ParsedSearchTerm{*word, parse_search_term(query.substr(word->begin, word->end - word->begin))};
  if (parsed.term.field != SearchField::Text && parsed.term.value.empty() && !parsed.term.quoted) {
    if (const auto value_word = next_search_word(query, word->end)) {
      const auto value = parse_search_term(query.substr(value_word->begin, value_word->end - value_word->begin));
      if (value.field == SearchField::Text && !value.exclude) {
        parsed.term.value = value.value;
        parsed.term.quoted = value.quoted;
        parsed.word.end = value_word->end;
      }
    } else {
      parsed.word.end = query.size();
    }
  }
  return parsed;
}

struct ComponentSearch {
  std::span<const flecs::id> components = {};
  std::span<const ComponentSearchAlias* const> aliases = {};

  auto matches(this const ComponentSearch& self, flecs::entity entity) -> bool {
    return std::ranges::any_of(self.components, [entity](flecs::id component) { return entity.has(component); }) ||
           std::ranges::any_of(self.aliases, [entity](const auto* alias) { return alias->matches(entity); });
  }
};

static auto resolve_component_search(
  std::span<const flecs::id> registered_components, const SearchTerm& term, memory::ScopedStack& stack
) -> ComponentSearch {
  const auto matches_name = [&term](const c8* name) {
    if (!name) {
      return false;
    }
    if (!term.quoted) {
      return contains_search_text(name, term.value);
    }
    const auto candidate = std::string_view(name);
    return candidate.size() == term.value.size() && starts_with_query_prefix(candidate, term.value);
  };

  auto ids = stack.alloc<flecs::id>(registered_components.size());
  usize count = 0;
  for (const auto component : registered_components) {
    const auto component_entity = component.entity();
    const auto matches = term.quoted && term.value.contains("::") ? matches_name(component_entity.path().c_str())
                                                                  : matches_name(component_entity.name().c_str());
    if (matches) {
      ids[count++] = component;
    }
  }

  if (term.quoted && count != 0) {
    return {.components = ids.first(count)};
  }
  auto aliases = stack.alloc<const ComponentSearchAlias*>(COMPONENT_SEARCH_ALIASES.size());
  usize alias_count = 0;
  for (const auto& alias : COMPONENT_SEARCH_ALIASES) {
    if (matches_name(alias.name)) {
      aliases[alias_count++] = &alias;
    }
  }
  return {.components = ids.first(count), .aliases = aliases.first(alias_count)};
}

struct SearchClause {
  SearchTerm term;
  ComponentSearch components;
  bool enabled = true;

  auto matches(this const SearchClause& self, flecs::entity entity) -> bool {
    bool matches = false;
    switch (self.term.field) {
      case SearchField::Component: matches = self.components.matches(entity); break;
      case SearchField::State    : matches = entity.enabled() == self.enabled; break;
      case SearchField::Text     :
      case SearchField::Name     : matches = contains_search_text(entity.name().c_str(), self.term.value); break;
    }
    return matches != self.term.exclude;
  }
};

struct HierarchySearch {
  std::span<const SearchClause> clauses;
  const c8* error = nullptr;
  bool structured = false;
};

static auto compile_hierarchy_search(
  std::string_view query, std::span<const flecs::id> components, memory::ScopedStack& stack
) -> HierarchySearch {
  auto clauses = stack.alloc<SearchClause>(query.size() / 2 + 1);
  usize count = 0;
  auto search = HierarchySearch{};
  for (usize offset = 0; const auto parsed = next_search_term(query, offset);) {
    offset = parsed->word.end;
    search.structured = search.structured || parsed->term.field != SearchField::Text;
    clauses[count++].term = parsed->term;
  }
  if (!search.structured) {
    return search;
  }

  for (auto& clause : clauses.first(count)) {
    const auto& term = clause.term;
    if (term.value.empty()) {
      search.error = "Enter a value after the search prefix";
      return search;
    }
    if (term.field == SearchField::Component) {
      clause.components = resolve_component_search(components, term, stack);
    } else if (term.field == SearchField::State) {
      if (term.value.size() == 7 && starts_with_query_prefix(term.value, "enabled")) {
        clause.enabled = true;
      } else if (term.value.size() == 8 && starts_with_query_prefix(term.value, "disabled")) {
        clause.enabled = false;
      } else {
        search.error = "Use state:enabled or state:disabled";
        return search;
      }
    }
  }
  search.clauses = clauses.first(count);
  return search;
}

struct SearchSuggestion {
  const c8* name;
  const c8* description;
  bool prefix_match;
};

struct SearchCompletions {
  SearchWord word = {};
  SearchTerm term = {};
  std::span<const SearchSuggestion> items;
};

static auto component_completions(
  std::string_view query, usize cursor, std::span<const flecs::id> components, memory::ScopedStack& stack
) -> SearchCompletions {
  for (usize offset = 0; const auto parsed = next_search_term(query, offset);) {
    const auto& word = parsed->word;
    offset = word.end;
    if (cursor <= word.begin) {
      break;
    }
    if (cursor > word.end) {
      continue;
    }
    const auto typed = next_search_term(query.substr(0, cursor), word.begin);
    if (!typed || typed->term.field != SearchField::Component) {
      break;
    }
    const auto& term = typed->term;

    auto candidates = stack.alloc<SearchSuggestion>(components.size() + COMPONENT_SEARCH_ALIASES.size());
    usize count = 0;
    const auto add = [&](const c8* name, const c8* description) {
      if (name && name[0] && (term.value.empty() || contains_search_text(name, term.value))) {
        candidates[count++] = {name, description, starts_with_query_prefix(name, term.value)};
      }
    };
    for (const auto component : components) {
      add(component.entity().name().c_str(), nullptr);
    }
    for (const auto& alias : COMPONENT_SEARCH_ALIASES) {
      add(alias.name, alias.description);
    }

    auto items = candidates.first(count);
    std::ranges::sort(items, [](const SearchSuggestion& a, const SearchSuggestion& b) {
      if (a.prefix_match != b.prefix_match) {
        return a.prefix_match;
      }
      return ImStricmp(b.name, a.name) < 0;
    });
    const auto duplicates = std::ranges::unique(items, [](const SearchSuggestion& a, const SearchSuggestion& b) {
      return ImStricmp(a.name, b.name) == 0;
    });
    constexpr usize max_suggestions = 6;
    return {word, term, items.first(std::min(max_suggestions, static_cast<usize>(duplicates.begin() - items.begin())))};
  }
  return {};
}

static auto complete_search_word(
  const SearchCompletions& completions, usize item, bool at_end, memory::ScopedStack& stack
) -> std::string_view {
  const auto name = std::string_view(completions.items[item].name);
  const auto quote = completions.term.quoted || name.find_first_of(SEARCH_WHITESPACE) != std::string_view::npos ? "\""
                                                                                                                : "";
  return stack.format(
    "{}{}{}{}{}{}",
    completions.term.exclude ? "-" : "",
    COMPONENT_SEARCH_PREFIX,
    quote,
    name,
    quote,
    at_end ? " " : ""
  );
}

struct SearchCompletionInput {
  std::span<const flecs::id> components;
  usize& selected;
};

static auto hierarchy_input_callback(ImGuiInputTextCallbackData* data) -> int {
  auto& input = *static_cast<SearchCompletionInput*>(data->UserData);
  memory::ScopedStack stack;
  const auto query = std::string_view(data->Buf, static_cast<usize>(data->BufTextLen));
  const auto completions = component_completions(query, static_cast<usize>(data->CursorPos), input.components, stack);
  if (completions.items.empty()) {
    input.selected = 0;
    return 0;
  }

  input.selected = std::min(input.selected, completions.items.size() - 1);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    if (data->EventKey == ImGuiKey_UpArrow) {
      input.selected = (input.selected + completions.items.size() - 1) % completions.items.size();
    } else {
      input.selected = (input.selected + 1) % completions.items.size();
    }
    return 0;
  }

  const auto [begin, end] = completions.word;
  const auto replacement = complete_search_word(completions, input.selected, end == query.size(), stack);
  if (query.size() - (end - begin) + replacement.size() < static_cast<usize>(data->BufSize)) {
    data->DeleteChars(static_cast<int>(begin), static_cast<int>(end - begin));
    data->InsertChars(static_cast<int>(begin), replacement.data(), replacement.data() + replacement.size());
    input.selected = 0;
  }
  return 0;
}

static auto has_mesh_in_hierarchy(flecs::entity entity) -> bool {
  if (entity.has<MeshComponent>()) {
    return true;
  }

  auto found = false;
  entity.children([&found](flecs::entity child) { found = found || has_mesh_in_hierarchy(child); });
  return found;
}

static auto add_mesh_colliders_to_hierarchy(flecs::entity entity) -> void {
  if (entity.has<MeshComponent>() && !entity.has<MeshColliderComponent>()) {
    entity.add<MeshColliderComponent>();
  }

  entity.children([](flecs::entity child) { add_mesh_colliders_to_hierarchy(child); });
}

static auto is_script_selected(const EditorContext& context) -> bool {
  return context.type == EditorContext::Type::Script && context.str.has_value();
}

static auto remove_selected_script(Scene& scene, EditorContext& context) -> void {
  scene.remove_script(ScriptKey{.type_name = context.str.value()});
  context.reset();
}

auto SceneHierarchyPanel::SelectedEntity::set(this SelectedEntity& self, flecs::entity e) -> void {
  self.entity = e;
  App::mod<Editor>().get_context().reset(EditorContext::Type::Entity, nullopt, e);
}

auto SceneHierarchyPanel::SelectedEntity::get(this SelectedEntity& self) -> flecs::entity {
  if (!self.entity.is_alive()) {
    self.entity = flecs::entity::null();
  }

  return self.entity;
}

auto SceneHierarchyPanel::SelectedEntity::reset(this SelectedEntity& self) -> void {
  self.entity = flecs::entity::null();
  App::mod<Editor>().get_context().reset();
}

SceneHierarchyPanel::SceneHierarchyPanel() : EditorPanelState("Scene Hierarchy", ICON_MDI_VIEW_LIST, true) {}

auto SceneHierarchyPanel::on_update(this SceneHierarchyPanel& self) -> void {
  auto& editor = App::mod<Editor>();
  auto& editor_context = editor.get_context();
  auto& undo_redo_system = editor.undo_redo_system;

  if (editor_context.type == EditorContext::Type::Entity) {
    if (editor_context.entity.has_value())
      self.selected_entity_.set(editor_context.entity.value());
  } else {
    self.selected_entity_.entity = flecs::entity::null();
  }

  const auto selected = self.selected_entity_.get();
  if (selected != flecs::entity::null()) {
    if (auto* cam = selected.try_get<CameraComponent>(); cam && self.scene_) {
      const auto proj = cam->get_projection_matrix() * cam->get_view_matrix();
      self.scene_->debug_renderer.draw_frustum(proj, glm::vec4(0, 1, 0, 1), cam->near_clip, cam->far_clip);
    }
    if (auto* light = selected.try_get<LightComponent>(); light && self.scene_ && selected.has<TransformComponent>()) {
      constexpr auto color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
      if (light->type == LightComponent::Point) {
        self.scene_->debug_renderer.draw_sphere(light->radius, Scene::get_world_position(selected), color);
      } else if (light->type == LightComponent::Spot && light->radius > 0.0f) {
        const auto world_transform = Scene::get_world_transform(selected);
        const auto forward = -glm::vec3(world_transform[2]);
        if (glm::dot(forward, forward) > 0.0f) {
          const auto direction = glm::normalize(forward);
          const auto rotation = glm::rotation(glm::vec3(0.0f, 0.0f, 1.0f), direction);
          const auto position = glm::vec3(world_transform[3]);
          auto& debug_renderer = self.scene_->debug_renderer;

          const auto draw_cutoff = [&](f32 cutoff, const glm::vec4& cone_color) {
            // use the shader's cosine cutoff and end the rim at the light's radial range
            const auto half_angle = glm::acos(glm::cos(cutoff));
            if (half_angle == 0.0f) {
              debug_renderer.draw_line(position, position + direction * light->radius, 1.0f, cone_color);
              return;
            }

            const auto length = light->radius * glm::cos(half_angle);
            debug_renderer.draw_cone(32, 4, 2.0f * half_angle, length, position, rotation, cone_color);
          };
          draw_cutoff(light->outer_cone_angle, color);
          draw_cutoff(light->inner_cone_angle, glm::vec4(1.0f, 0.85f, 0.0f, 1.0f));
        }
      }
    }

    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D) && self.scene_) {
      self.selected_entity_.set(self.scene_->duplicate_entity(self.selected_entity_.get()));
    }
    if (
      ImGui::IsKeyPressed(ImGuiKey_Delete) && (self.table_hovered_ || editor.main_viewport_panel.get_focused_viewport())
    ) {
      self.deleted_entity_ = self.selected_entity_.get();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
      self.renaming_entity_ = self.selected_entity_.get();
    }
  }

  if (
    self.scene_ && is_script_selected(editor_context) && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
    (self.table_hovered_ || self.table_hovered_scripts)
  ) {
    remove_selected_script(*self.scene_, editor_context);
  }

  if (self.deleted_entity_) {
    auto command_id = fmt::format("delete entity {}", self.deleted_entity_.name().c_str());
    undo_redo_system->execute_command<EntityDeleteCommand>(self.scene_, self.deleted_entity_, "", command_id);
    self.selected_entity_.reset();
  }
}

auto SceneHierarchyPanel::draw_entity_search(this SceneHierarchyPanel& self) -> void {
  const auto button_size = ImGui::GetFrameHeight();
  const auto buttons_width = 2.0f * (button_size + ImGui::GetStyle().ItemSpacing.x);
  auto& filter = self.entities_filter_;
  const auto input_id = ImGui::GetID("###HierarchyFilter");
  if (self.focus_search_) {
    if (auto* state = ImGui::GetInputTextState(input_id)) {
      state->ReloadUserBufAndMoveToEnd();
    }
    ImGui::SetKeyboardFocusHere();
    self.focus_search_ = false;
  }

  auto completion_input = SearchCompletionInput{self.scene_->component_db.get_components(), self.selected_suggestion_};
  constexpr auto input_flags = ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
  ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttons_width));
  if (
    ImGui::InputTextWithHint(
      "###HierarchyFilter",
      ICON_MDI_MAGNIFY " Search name or component:...",
      filter.InputBuf,
      sizeof(filter.InputBuf),
      input_flags,
      hierarchy_input_callback,
      &completion_input
    )
  ) {
    filter.Build();
    self.selected_suggestion_ = 0;
  }

  const auto show_suggestions = ImGui::IsItemActive() || ImGui::IsItemDeactivated();
  ImGui::SameLine();
  draw_component_search_help(button_size);
  ImGui::SameLine();
  if (UI::button(ICON_MDI_PLUS "###AddEntity", {button_size, button_size})) {
    ImGui::OpenPopup("scene_h_entities_context_window");
  }

  if (!show_suggestions) {
    self.selected_suggestion_ = 0;
    return;
  }
  const auto* input_state = ImGui::GetInputTextState(input_id);
  if (!input_state) {
    return;
  }

  memory::ScopedStack stack;
  const auto query = std::string_view(filter.InputBuf);
  const auto cursor = std::min(query.size(), static_cast<usize>(input_state->GetCursorPos()));
  const auto completions = component_completions(query, cursor, completion_input.components, stack);
  if (completions.items.empty()) {
    self.selected_suggestion_ = 0;
    return;
  }

  self.selected_suggestion_ = std::min(self.selected_suggestion_, completions.items.size() - 1);
  for (usize i = 0; i < completions.items.size(); ++i) {
    const auto& item = completions.items[i];
    const auto* label = item.description ? stack.format_char("{}  ({})", item.name, item.description) : item.name;
    constexpr auto flags = ImGuiSelectableFlags_SelectOnClick | ImGuiSelectableFlags_NoHoldingActiveID;
    if (ImGui::Selectable(label, i == self.selected_suggestion_, flags)) {
      const auto [begin, end] = completions.word;
      const auto replacement = complete_search_word(completions, i, end == query.size(), stack);
      const auto updated = stack.format("{}{}{}", query.substr(0, begin), replacement, query.substr(end));
      if (updated.size() < sizeof(filter.InputBuf)) {
        ImStrncpy(filter.InputBuf, updated.data(), sizeof(filter.InputBuf));
        filter.Build();
        self.selected_suggestion_ = 0;
        self.focus_search_ = true;
      }
      break;
    }
    UI::tooltip_hover(item.description);
  }
}

auto SceneHierarchyPanel::on_render(this SceneHierarchyPanel& self, const SwapchainInfo& swapchain_info) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  ImGuiScoped::StyleVar cellpad(ImGuiStyleVar_CellPadding, {0, 0});

  if (self.focus_search_) {
    ImGui::SetNextWindowFocus();
  }
  if (ImGui::Begin(self.id.c_str(), &self.visible, ImGuiWindowFlags_NoCollapse)) {
    if (!self.scene_) {
      const auto warning_text = "No scene!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - text_width) * 0.5f);
      ImGui::SetCursorPosY(ImGui::GetContentRegionAvail().y * 0.5f);
      ImGui::Text(warning_text);

      ImGui::End();
      return;
    }

    constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_BordersInner |
                                            ImGuiTableFlags_ScrollY;
    const float line_height = ImGui::GetTextLineHeight();

    float filter_cursor_pos_x = ImGui::GetCursorPosX();
    if (ImGui::TreeNodeEx("Scripts", ImGuiTreeNodeFlags_Framed)) {
      self.scripts_filter_.Draw(
        "###HierarchyFilter",
        ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(ICON_MDI_PLUS).x + UI::scale(20.0f))
      );
      ImGui::SameLine();

      if (UI::button(stack.format_char("{}###AddScript", ICON_MDI_PLUS)))
        ImGui::OpenPopup("scene_h_scripts_context_window");

      if (
        ImGui::BeginPopupContextWindow(
          "scene_h_scripts_context_window",
          ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
        )
      ) {
        self.draw_scripts_context_menu();
        ImGui::EndPopup();
      }

      if (!self.scripts_filter_.IsActive()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
        auto search_txt = fmt::format("  {} Search scripts...", ICON_MDI_MAGNIFY);
        ImGui::TextUnformatted(search_txt.c_str());
      }

      if (ImGui::BeginTable("ScriptsTable", 1, table_flags, ImVec2(0.0f, UI::scale(100.0f)))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip);

        auto& editor_context = App::mod<Editor>().get_context();

        for (const auto& [key, script] : self.scene_->get_scripts()) {
          auto system_name = script->display_name();
          if (self.scripts_filter_.IsActive() && !self.scripts_filter_.PassFilter(system_name.c_str())) {
            continue;
          }

          ImGui::TableNextRow();

          ImGui::TableNextColumn();

          const auto is_selected = is_script_selected(editor_context) && editor_context.str.value() == key.type_name;

          ImGuiTreeNodeFlags flags = (is_selected ? ImGuiTreeNodeFlags_Selected : 0);
          flags |= ImGuiTreeNodeFlags_OpenOnArrow;
          flags |= ImGuiTreeNodeFlags_SpanFullWidth;
          flags |= ImGuiTreeNodeFlags_FramePadding;
          flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

          const bool highlight = is_selected;
          if (highlight) {
            ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
            ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(active_color));
            ImGui::PushStyleColor(ImGuiCol_Header, active_color);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, is_selected ? active_color : hovered_color);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
          }

          ImGui::TreeNodeEx(script.get(), flags, "%s %s", ICON_MDI_SCRIPT, system_name.c_str());

          if (highlight)
            ImGui::PopStyleColor(3);

          // Select
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            self.selected_entity_.reset();
            editor_context.reset(EditorContext::Type::Script, key.type_name);
          }
        }

        if (
          ImGui::BeginPopupContextWindow(
            "scene_h_scripts_context_window",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
          )
        ) {
          self.selected_entity_.reset();
          self.draw_scripts_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Remove", "Del")) {
            if (is_script_selected(editor_context)) {
              remove_selected_script(*self.scene_, editor_context);
            }
          }

          ImGui::EndPopup();
        }

        self.table_hovered_scripts = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          self.selected_entity_.reset();
        }
      }
      ImGui::TreePop();
    }

    if (self.focus_search_) {
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    if (ImGui::TreeNodeEx("Entities", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      self.draw_entity_search();

      if (self.search_root_ && !self.search_root_.is_alive()) {
        self.search_root_ = {};
      }
      if (self.search_root_) {
        if (ImGui::SmallButton("Clear Scope")) {
          self.search_root_ = {};
        } else {
          ImGui::SameLine();
          ImGui::Text("Searching %s", self.search_root_.name().c_str());
          if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(self.search_root_.path().c_str());
            ImGui::EndTooltip();
          }
        }
      }

      if (
        ImGui::BeginPopupContextWindow(
          "scene_h_entities_context_window",
          ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
        )
      ) {
        self.draw_entities_context_menu();
        ImGui::EndPopup();
      }

      const ImVec2 cursor_pos = ImGui::GetCursorPos();
      const ImVec2 region = ImGui::GetContentRegionAvail();
      if (region.x != 0.0f && region.y != 0.0f) {
        ImGui::InvisibleButton("##DragDropTargetBehindTable", region);
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
            const auto* payload_data = PayloadData::from_payload(payload);
            const auto path = payload_data->get_path();
            if (path.extension() == ".oxparticle") {
              auto asset = import_asset(App::mod<AssetManager>(), path);
              if (asset) {
                auto new_entity = self.scene_->create_particle_system_entity(asset);
                if (new_entity != flecs::entity::null()) {
                  self.selected_entity_.set(new_entity);
                }
              }
            }
          }
          ImGui::EndDragDropTarget();
        }
      }

      ImGui::SetCursorPos(cursor_pos);
      if (ImGui::BeginTable("HierarchyTable", 3, table_flags)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, line_height * 3.0f);

        const auto vis_colon = fmt::format("  {}", ICON_MDI_EYE_OUTLINE);
        ImGui::TableSetupColumn(vis_colon.c_str(), ImGuiTableColumnFlags_WidthFixed, line_height * 2.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const auto query = compile_hierarchy_search(
          self.entities_filter_.InputBuf,
          self.scene_->component_db.get_components(),
          stack
        );
        const auto visit_roots = [&self](auto&& visit) {
          if (self.search_root_) {
            visit(self.search_root_);
            return;
          }
          self.scene_->world.query_builder<const TransformComponent>()
            .with(flecs::Disabled)
            .optional()
            .build()
            .each([&visit](flecs::entity entity, const TransformComponent&) {
              if (entity.parent() == flecs::entity::null()) {
                visit(entity);
              }
            });
        };
        if (query.structured || self.search_root_) {
          bool found_match = false;
          const auto draw_matches = [&](this const auto& visit, flecs::entity entity) -> void {
            if (entity.has<Hidden>()) {
              return;
            }
            const auto matches = query.structured
                                   ? std::ranges::all_of(
                                       query.clauses,
                                       [entity](const SearchClause& clause) { return clause.matches(entity); }
                                     )
                                   : self.entities_filter_.PassFilter(entity.name().c_str());
            if (matches) {
              ImGuiScoped::ID result_id(reinterpret_cast<const void*>(entity.raw_id()));
              self.draw_entity_node(entity, EntityView::SearchResult);
              found_match = true;
            }
            entity.children([&visit](flecs::entity child) { visit(child); });
          };
          if (!query.error) {
            visit_roots(draw_matches);
          }
          if (!found_match) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", query.error ? query.error : "No matching entities");
          }
        } else {
          visit_roots([&self](flecs::entity entity) { self.draw_entity_node(entity); });
        }
        ImGui::PopStyleVar();

        if (
          ImGui::BeginPopupContextWindow(
            "scene_h_entities_context_window",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
          )
        ) {
          self.selected_entity_.reset();
          self.draw_entities_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        self.table_hovered_ = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          self.selected_entity_.reset();
        }
      }

      if (ImGui::IsMouseDown(0) && self.window_hovered_) {
        self.selected_entity_.reset();
      }

      if (self.dragged_entity_ != flecs::entity::null() && self.dragged_entity_target_ != flecs::entity::null()) {
        self.dragged_entity_.child_of(self.dragged_entity_target_);
        self.dragged_entity_ = flecs::entity::null();
        self.dragged_entity_target_ = flecs::entity::null();
      }

      ImGui::TreePop();
    }

    self.window_hovered_ = ImGui::IsWindowHovered();
  }

  ImGui::End();
}

auto SceneHierarchyPanel::draw_entity_node(
  this SceneHierarchyPanel& self,
  flecs::entity entity,
  EntityView view,
  u32 depth,
  bool force_expand_tree,
  bool is_part_of_prefab
) -> ImRect {
  ZoneScoped;

  if (entity.has<Hidden>())
    return {0, 0, 0, 0};

  ImGui::TableNextRow();
  ImGui::TableNextColumn();

  const auto is_search_result = view == EntityView::SearchResult;
  const auto child_count = is_search_result ? 0 : self.scene_->world.count(flecs::ChildOf, entity);

  if (
    !is_search_result && self.entities_filter_.IsActive() && !self.entities_filter_.PassFilter(entity.name().c_str())
  ) {
    entity.children([&self](flecs::entity child) { self.draw_entity_node(child); });
    return {0, 0, 0, 0};
  }

  const auto is_selected = self.selected_entity_.get().id() == entity.id();

  ImGuiTreeNodeFlags flags = (is_selected ? ImGuiTreeNodeFlags_Selected : 0);
  flags |= ImGuiTreeNodeFlags_OpenOnArrow;
  flags |= ImGuiTreeNodeFlags_SpanFullWidth;
  flags |= ImGuiTreeNodeFlags_FramePadding;

  if (child_count == 0) {
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }

  if (is_selected) {
    ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
    ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(active_color));
    ImGui::PushStyleColor(ImGuiCol_Header, active_color);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, is_selected ? active_color : hovered_color);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }

  if (force_expand_tree)
    ImGui::SetNextItemOpen(true);

  // const bool prefab_color_applied = is_part_of_prefab && !is_selected;
  // if (prefab_color_applied)
  //   ImGui::PushStyleColor(ImGuiCol_Text, header_selected_color);

  const bool opened = ImGui::TreeNodeEx(
    reinterpret_cast<void*>(entity.raw_id()),
    flags,
    "%s %s",
    ICON_MDI_CUBE_OUTLINE,
    entity.name().c_str()
  );

  if (is_selected)
    ImGui::PopStyleColor(3);

  // Select
  if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    self.selected_entity_.set(entity);
  }

  if (is_search_result && ImGui::BeginItemTooltip()) {
    ImGui::TextUnformatted(entity.path().c_str());
    ImGui::EndTooltip();
  }

  // Expand recursively
  if (ImGui::IsItemToggledOpen() && (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
    force_expand_tree = opened;

  bool entity_deleted = false;

  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Search This Subtree")) {
      self.search_root_ = entity;
      self.focus_search_ = true;
    }
    if (ImGui::MenuItem("Rename", "F2"))
      self.renaming_entity_ = entity;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
      self.selected_entity_.set(self.scene_->duplicate_entity(entity));
    }
    if (ImGui::MenuItem("Delete", "Del"))
      entity_deleted = true;

    if (has_mesh_in_hierarchy(entity) && ImGui::MenuItem("Add Mesh Colliders")) {
      // deferred so adding components doesn't move entities out from under the children() walk
      entity.world().defer([&entity] {
        add_mesh_colliders_to_hierarchy(entity);
        if (!entity.has<RigidBodyComponent>()) {
          entity.set<RigidBodyComponent>({.type = RigidBodyComponent::Static});
        }
      });
    }

    ImGui::Separator();

    self.draw_entities_context_menu();

    ImGui::EndPopup();
  }

  ImVec2 vertical_line_start = ImGui::GetCursorScreenPos();
  vertical_line_start.x -= 0.5f;
  vertical_line_start.y -= ImGui::GetFrameHeight() * 0.5f;

  // Drag Drop
  {
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* entity_payload = ImGui::AcceptDragDropPayload("Entity")) {
        self.dragged_entity_ = *static_cast<flecs::entity*>(entity_payload->Data);
        self.dragged_entity_target_ = entity;
      } else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
        const auto* payload_data = PayloadData::from_payload(payload);
        const auto path = payload_data->get_path();
        if (path.extension() == ".oxparticle") {
          auto asset = import_asset(App::mod<AssetManager>(), path);
          if (asset) {
            auto new_entity = self.scene_->create_particle_system_entity(asset);
            if (new_entity != flecs::entity::null()) {
              new_entity.child_of(entity);
              self.selected_entity_.set(new_entity);
            }
          }
        }
      }

      ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("Entity", &entity, sizeof(flecs::entity));
      ImGui::TextUnformatted(entity.name().c_str());
      ImGui::EndDragDropSource();
    }
  }

  if (entity.id() == self.renaming_entity_.id()) {
    static bool renaming = false;
    if (!renaming) {
      renaming = true;
      ImGui::SetKeyboardFocusHere();
    }

    std::string name{entity.name()};
    if (ImGui::InputText("##Tag", &name)) {
      entity.set_name(name.c_str());
    }

    if (ImGui::IsItemDeactivated()) {
      renaming = false;
      self.renaming_entity_ = flecs::entity::null();
    }
  }

  ImGui::TableNextColumn();

  ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0, 0, 0, 0});

  const float button_size_x = ImGui::GetContentRegionAvail().x;
  const float frame_height = ImGui::GetFrameHeight();
  ImGui::PushID(entity.name());
  ImGui::Button(is_part_of_prefab ? "Prefab" : "Entity", {button_size_x, frame_height});
  ImGui::PopID();
  // Select
  if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
    self.selected_entity_.set(entity);
  }

  ImGui::TableNextColumn();
  // Visibility Toggle
  {
    ImGui::Text("  %s", entity.enabled() ? ICON_MDI_EYE_OUTLINE : ICON_MDI_EYE_OFF_OUTLINE);

    if (ImGui::IsItemHovered() && (ImGui::IsMouseDragging(0) || ImGui::IsItemClicked())) {
      entity.enabled() ? entity.disable() : entity.enable();
    }
  }

  ImGui::PopStyleColor(3);

  // if (prefab_color_applied)
  //   ImGui::PopStyleColor();

  // Open
  const ImRect node_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  {
    if (opened && child_count > 0 && !entity_deleted) {
      ImColor tree_line_color;
      depth %= 4;
      switch (depth) {
        case 0 : tree_line_color = ImColor(254, 112, 246); break;
        case 1 : tree_line_color = ImColor(142, 112, 254); break;
        case 2 : tree_line_color = ImColor(112, 180, 254); break;
        case 3 : tree_line_color = ImColor(48, 134, 198); break;
        default: tree_line_color = ImColor(255, 255, 255); break;
      }

      entity.children([&self, depth, force_expand_tree, is_part_of_prefab, vertical_line_start, tree_line_color](
                        const flecs::entity child
                      ) {
        const float horizontal_tree_line_size = UI::scale(
          self.scene_->world.count(flecs::ChildOf, child) > 0 ? 9.0f : 18.0f
        );
        // chosen arbitrarily
        const ImRect child_rect = self.draw_entity_node(
          child,
          EntityView::Hierarchy,
          depth + 1,
          force_expand_tree,
          is_part_of_prefab
        );

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 vertical_line_end = vertical_line_start;
        const float line_thickness = UI::scale(1.5f);
        const float midpoint = (child_rect.Min.y + child_rect.Max.y) / 2.0f;
        draw_list->AddLine(
          ImVec2(vertical_line_start.x, midpoint),
          ImVec2(vertical_line_start.x + horizontal_tree_line_size, midpoint),
          tree_line_color,
          line_thickness
        );
        vertical_line_end.y = midpoint;
        draw_list->AddLine(vertical_line_start, vertical_line_end, tree_line_color, line_thickness);
      });
    }

    if (opened && child_count > 0)
      ImGui::TreePop();
  }

  // PostProcess Actions
  if (entity_deleted)
    self.deleted_entity_ = entity;

  return node_rect;
}

auto SceneHierarchyPanel::draw_entities_context_menu(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;

  const bool has_context = self.selected_entity_.get() != flecs::entity::null();

  flecs::entity to_select = flecs::entity::null();

  ImGuiScoped::StyleVar styleVar1(ImGuiStyleVar_ItemInnerSpacing, UI::scale(ImVec2(0.0f, 5.0f)));
  ImGuiScoped::StyleVar styleVar2(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(1.0f, 5.0f)));
  if (ImGui::BeginMenu("Create")) {
    if (ImGui::MenuItem("New Entity")) {
      to_select = self.scene_->create_entity("entity", true);
    }

    if (ImGui::MenuItem("Sprite")) {
      to_select = self.scene_->create_entity("sprite", true).add<SpriteComponent>();
    }

    if (ImGui::MenuItem("Camera")) {
      to_select = self.scene_->create_entity("camera", true);
      to_select.add<CameraComponent>().get_mut<TransformComponent>().rotation.y = glm::radians(-90.f);
    }

    if (ImGui::MenuItem("Terrain")) {
      to_select = self.scene_->create_entity("terrain", true);
      to_select //
        .add<TransformComponent>()
        .set<TerrainComponent>({
          .domain_size = 0.7f,
          .height_frequency = 1.1f,
          .height_amplitude = 3.335f,
          .height_lacunarity = 0.4f,
          .height_octaves = 6,
          .seed = 246,
          .erosion_scale = 0.450f,
          .erosion_strength = 0.220f,
          .ridge_rounding = 0.2f,
          .crease_rounding = -0.2f,
          .erosion_octaves = 16,
        });
    }

    if (ImGui::MenuItem("Probe Volume")) {
      to_select = self.scene_->create_entity("probe_volume", true).add<ProbeVolumeComponent>();
    }

    if (ImGui::MenuItem("Decal")) {
      to_select = self.scene_->create_entity("decal", true).add<DecalComponent>();
      auto& decal_transform = to_select.get_mut<TransformComponent>();
      decal_transform.rotation = glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1.0f, 0.0f, 0.0f));
      decal_transform.scale = glm::vec3(2.0f, 2.0f, 1.0f);
    }

    if (ImGui::BeginMenu("Light")) {
      if (ImGui::MenuItem("Light")) {
        to_select = self.scene_->create_entity("light", true).add<LightComponent>();
      }
      if (ImGui::MenuItem("Sun")) {
        to_select = self.scene_->create_entity("sun", true)
                      .set<LightComponent>(LightComponent{.type = LightComponent::Directional, .intensity = 10.f})
                      .add<AtmosphereComponent>();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Audio")) {
      if (ImGui::MenuItem("Audio Source")) {
        to_select = self.scene_->create_entity("audio_source", true).add<AudioSourceComponent>();
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Audio Listener")) {
        to_select = self.scene_->create_entity("audio_listener", true).add<AudioListenerComponent>();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Effects")) {
      if (ImGui::MenuItem("Particle System")) {
        to_select = self.scene_->create_entity("particle_system", true).add<ParticleSystemComponent>();
      }
      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  if (has_context && to_select != flecs::entity::null())
    to_select.child_of(self.selected_entity_.get());

  if (to_select != flecs::entity::null()) {
    self.selected_entity_.set(to_select);
  }
}

auto SceneHierarchyPanel::draw_scripts_context_menu(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

#ifdef OX_DOTNET
  if (ImGui::BeginMenu(stack.format_char("{} Add C# script", ICON_MDI_LANGUAGE_CSHARP))) {
    auto is_any_listed = false;
    if (App::has_mod<DotNetScripting>()) {
      for (const auto& script_type : App::mod<DotNetScripting>().script_types) {
        if (script_type.kind != OxScriptKind::SceneBehaviour) {
          continue;
        }

        const auto is_in_scene = self.scene_->get_scripts().contains(ScriptKey{.type_name = script_type.name});
        if (is_in_scene) {
          continue;
        }

        is_any_listed = true;
        if (ImGui::MenuItem(script_type.name.c_str())) {
          if (auto result = self.scene_->add_script_type(script_type.name); !result) {
            OX_LOG_ERROR("{}", result.error());
          }
        }
      }
    }

    if (!is_any_listed) {
      ImGui::BeginDisabled();
      ImGui::TextUnformatted("C# scripts aren't built yet");
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }
#endif
}

auto SceneHierarchyPanel::set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void {
  ZoneScoped;

  self.current_scene = scene;
  self.scene_ = scene != nullptr ? scene->get_scene().get() : nullptr;
  self.search_root_ = {};

  self.selected_entity_.reset();
  self.renaming_entity_ = {};
  self.dragged_entity_ = {};
  self.dragged_entity_target_ = {};
  self.deleted_entity_ = {};
}

auto SceneHierarchyPanel::find_component(this SceneHierarchyPanel& self, flecs::entity component) -> void {
  if (!self.scene_ || component.world().c_ptr() != self.scene_->world.c_ptr()) {
    return;
  }

  const auto name = component.name();
  const auto components = self.scene_->component_db.get_components();
  const auto ambiguous = std::ranges::any_of(components, [&](flecs::id other) {
    return other != component && ImStricmp(other.entity().name().c_str(), name.c_str()) == 0;
  });

  memory::ScopedStack stack;
  const auto path = ambiguous ? component.path() : flecs::string{};
  const auto query = stack.format("{}\"{}\"", COMPONENT_SEARCH_PREFIX, ambiguous ? path.c_str() : name.c_str());
  if (query.size() < sizeof(self.entities_filter_.InputBuf)) {
    ImStrncpy(self.entities_filter_.InputBuf, query.data(), sizeof(self.entities_filter_.InputBuf));
    self.entities_filter_.Build();
    self.search_root_ = {};
    self.selected_suggestion_ = 0;
    self.focus_search_ = true;
    self.visible = true;
  }
}

auto SceneHierarchyPanel::get_scene(this const SceneHierarchyPanel& self) -> EditorScene* {
  ZoneScoped;

  return self.current_scene;
}
} // namespace ox
