#include <RmlUi/Core.h>
#include <algorithm>
#include <ankerl/svector.h>
#include <ankerl/unordered_dense.h>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "Core/VFS.hpp"
#include "DotNetHosts.hpp"
#include "Memory/Stack.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto to_view(OxStringView view) -> std::string_view { return std::string_view(view.data, view.length); }

static auto to_string(OxStringView view) -> Rml::String { return Rml::String(view.data, view.length); }

static auto to_ox(const Rml::String& text) -> OxStringView {
  return OxStringView{.data = text.data(), .length = text.size()};
}

// handles hand out an element through RmlUi's observer, so one RmlUi has freed comes back null rather than dangling
static auto elements = ankerl::unordered_dense::map<u64, Rml::ObserverPtr<Rml::Element>>{};
static auto element_handles = ankerl::unordered_dense::map<Rml::Element*, u64>{};
static auto next_element = 1_u64;
// how many the last sweep left. every event hands its targets a handle, so the maps are swept again once they double
static auto swept_elements = 0_sz;
static auto contexts = ankerl::unordered_dense::map<ecs_world_t*, Rml::Context*>{};

// elements a script removed. a dispatch still hands one to the listeners further up its path after the script that
// removed it returns, so they go once no RmlUi call is on the stack: the next script call outside a callback, or the
// next frame
static auto graveyard = std::vector<Rml::ElementPtr>{};
static auto managed_depth = 0_u32;

struct ManagedCall {
  ManagedCall() { managed_depth++; }

  ~ManagedCall() { managed_depth--; }

  ManagedCall(const ManagedCall&) = delete;
  auto operator=(const ManagedCall&) -> ManagedCall& = delete;
};

// forgets freed elements, and every element in `context` when there is one
static auto sweep_elements(Rml::Context* context) -> void {
  ZoneScoped;

  std::erase_if(elements, [context](const auto& entry) {
    auto* element = entry.second.get();
    return !element || (context && element->GetContext() == context);
  });
  std::erase_if(element_handles, [](const auto& entry) {
    const auto it = elements.find(entry.second);
    return it == elements.end() || it->second.get() != entry.first;
  });
  swept_elements = elements.size();
}

static auto handle_of(Rml::Element* element) -> u64 {
  if (!element) {
    return 0;
  }

  // a freed element's address can come back as a new one, so a stale entry for it is replaced
  if (const auto it = element_handles.find(element); it != element_handles.end()) {
    const auto existing = elements.find(it->second);
    if (existing != elements.end() && existing->second.get() == element) {
      return it->second;
    }
  }

  if (elements.size() >= 2 * std::max(swept_elements, 256_sz)) {
    sweep_elements(nullptr);
  }

  const auto handle = next_element++;
  elements[handle] = element->GetObserverPtr();
  element_handles[element] = handle;
  return handle;
}

auto collect_removed_ui() -> void {
  if (managed_depth == 0 && !graveyard.empty()) {
    auto dead = std::move(graveyard);
    graveyard.clear();
  }
}

static auto element_of(u64 handle) -> Rml::Element* {
  collect_removed_ui();

  const auto it = elements.find(handle);
  if (it == elements.end()) {
    return nullptr;
  }

  if (auto* element = it->second.get()) {
    return element;
  }

  elements.erase(it);
  return nullptr;
}

static auto context_of(ecs_world_t* world) -> Rml::Context* {
  if (const auto it = contexts.find(world); it != contexts.end()) {
    return it->second;
  }

  auto* scene = scene_of(world);
  return scene ? scene->get_rml_context() : nullptr;
}

static auto to_event(Rml::Event& event, const Rml::String& type) -> OxUiEvent {
  const auto mouse = event.GetUnprojectedMouseScreenPos();
  return OxUiEvent{
    .type = to_ox(type),
    .target = handle_of(event.GetTargetElement()),
    .current = handle_of(event.GetCurrentElement()),
    .mouse_x = mouse.x,
    .mouse_y = mouse.y,
    .event = &event,
  };
}

class ManagedListener final : public Rml::EventListener {
public:
  u64 id = 0;
  // what release_ui(world) matches it by, a context outlives every element in it
  Rml::Context* context = nullptr;
  Rml::ObserverPtr<Rml::Element> element = {};
  Rml::String event = {};

  auto ProcessEvent(Rml::Event& rml_event) -> void override {
    const auto* managed = managed_api();
    if (!managed) {
      return;
    }

    const ManagedCall call;
    const auto ox_event = to_event(rml_event, rml_event.GetType());
    managed->ui_event(id, &ox_event);
  }
};

static auto to_variant(const OxValue& value) -> Rml::Variant;

static auto read_list_value(u64 model, i32 binding, i32 index, i32 field, Rml::Variant& variant) -> bool {
  const auto* managed = managed_api();
  if (!managed) {
    return false;
  }

  // a string's text is managed memory that stays pinned only until the next call, so it is copied right away
  const ManagedCall call;
  auto value = OxValue{};
  managed->ui_model_list_get(model, binding, index, field, &value);
  variant = to_variant(value);
  return true;
}

static auto list_index(void* ptr) -> i32 { return static_cast<i32>(reinterpret_cast<uintptr_t>(ptr)); }

class ListFieldDefinition final : public Rml::VariableDefinition {
public:
  u64 model = 0;
  i32 binding = 0;
  i32 field = 0;

  ListFieldDefinition(u64 model_id, i32 list_binding, i32 field_index)
      : VariableDefinition(Rml::DataVariableType::Scalar),
        model(model_id),
        binding(list_binding),
        field(field_index) {}

  auto Get(void* ptr, Rml::Variant& variant) -> bool override {
    return read_list_value(model, binding, list_index(ptr), field, variant);
  }

  auto Set(void*, const Rml::Variant&) -> bool override { return false; }
};

class ListItemDefinition final : public Rml::VariableDefinition {
public:
  u64 model = 0;
  i32 binding = 0;
  ankerl::svector<Rml::String, 4> names = {};
  ankerl::svector<Rml::VariableDefinition*, 4> fields = {};

  ListItemDefinition(u64 model_id, i32 list_binding, bool is_scalar)
      : VariableDefinition(is_scalar ? Rml::DataVariableType::Scalar : Rml::DataVariableType::Struct),
        model(model_id),
        binding(list_binding) {}

  auto Get(void* ptr, Rml::Variant& variant) -> bool override {
    if (Type() != Rml::DataVariableType::Scalar) {
      return false;
    }

    return read_list_value(model, binding, list_index(ptr), -1, variant);
  }

  auto Set(void*, const Rml::Variant&) -> bool override { return false; }

  auto Child(void* ptr, const Rml::DataAddressEntry& address) -> Rml::DataVariable override {
    const auto it = std::ranges::find(names, address.name);
    if (it == names.end()) {
      return Rml::DataVariable();
    }

    return Rml::DataVariable(fields[static_cast<usize>(it - names.begin())], ptr);
  }

  auto ReflectMemberNames() -> Rml::StringList override { return Rml::StringList(names.begin(), names.end()); }
};

class ListDefinition final : public Rml::VariableDefinition {
public:
  u64 model = 0;
  i32 binding = 0;
  Rml::VariableDefinition* item = nullptr;

  ListDefinition(u64 model_id, i32 list_binding, Rml::VariableDefinition* item_definition)
      : VariableDefinition(Rml::DataVariableType::Array),
        model(model_id),
        binding(list_binding),
        item(item_definition) {}

  auto Size(void*) -> int override {
    const auto* managed = managed_api();
    if (!managed) {
      return 0;
    }

    const ManagedCall call;
    return managed->ui_model_list_size(model, binding);
  }

  auto Child(void* ptr, const Rml::DataAddressEntry& address) -> Rml::DataVariable override {
    const auto size = Size(ptr);
    if (address.index < 0 || address.index >= size) {
      if (address.name == "size") {
        return Rml::MakeLiteralIntVariable(size);
      }

      return Rml::DataVariable();
    }

    return Rml::DataVariable(item, reinterpret_cast<void*>(static_cast<uintptr_t>(address.index)));
  }
};

struct ManagedModel {
  u64 id = 0;
  ecs_world_t* world = nullptr;
  Rml::Context* context = nullptr;
  Rml::String name = {};
  Rml::DataModelConstructor constructor = {};
  // what a setter hands managed code a string in, kept until the call returns
  Rml::String text = {};
  // what the model's lists read through, so they have to outlive its data
  std::vector<std::unique_ptr<Rml::VariableDefinition>> definitions = {};
};

static auto listeners = std::vector<std::unique_ptr<ManagedListener>>{};
static auto models = std::vector<std::unique_ptr<ManagedModel>>{};
static auto next_listener = 1_u64;
static auto next_model = 1_u64;

static auto model_of(u64 id) -> ManagedModel* {
  const auto it = std::ranges::find(models, id, [](const auto& model) { return model->id; });
  return it == models.end() ? nullptr : it->get();
}

static auto to_variant(const OxValue& value) -> Rml::Variant {
  switch (value.type) {
    case OxValueType::Bool  : return Rml::Variant(value.integer != 0);
    case OxValueType::U8    :
    case OxValueType::U16   :
    case OxValueType::I32   :
    case OxValueType::I64   : return Rml::Variant(static_cast<int64_t>(value.integer));
    case OxValueType::F32   :
    case OxValueType::F64   : return Rml::Variant(value.number);
    case OxValueType::String: return Rml::Variant(to_string(value.text));
    default                 : return Rml::Variant();
  }
}

static auto to_value(const Rml::Variant& variant, Rml::String& text) -> OxValue {
  switch (variant.GetType()) {
    case Rml::Variant::BOOL  : return OxValue{.type = OxValueType::Bool, .integer = variant.Get<bool>() ? 1 : 0};
    case Rml::Variant::INT   :
    case Rml::Variant::INT64 :
    case Rml::Variant::UINT  :
    case Rml::Variant::UINT64:
    case Rml::Variant::BYTE  :
    case Rml::Variant::CHAR  : return OxValue{.type = OxValueType::I64, .integer = variant.Get<int64_t>()};
    case Rml::Variant::FLOAT :
    case Rml::Variant::DOUBLE: return OxValue{.type = OxValueType::F64, .number = variant.Get<double>()};
    case Rml::Variant::NONE  : return OxValue{};
    default: text = variant.Get<Rml::String>(); return OxValue{.type = OxValueType::String, .text = to_ox(text)};
  }
}

// whatever writes the text back, RmlUi reads it before the next call into the model
static auto read_text(OxStringView view, c8* out, u32 capacity, u32* length) -> void {
  *length = static_cast<u32>(view.length);
  if (out && view.length) {
    std::memcpy(out, view.data, std::min<usize>(view.length, capacity));
  }
}

static auto ui_load_document(ecs_world_t* world, OxStringView path, u64* document) -> OxStatus {
  ZoneScoped;

  *document = 0;
  auto* context = context_of(world);
  if (!context) {
    return OxStatus::NotFound;
  }

  // the engine's file interface reads virtual paths, so gameplay names documents the way it names every asset
  const auto virtual_path = (std::filesystem::path(VFS::ASSETS_DIR) / to_view(path)).generic_string();
  auto* loaded = context->LoadDocument(virtual_path);
  if (!loaded) {
    return OxStatus::NotFound;
  }

  *document = handle_of(loaded);
  return OxStatus::Ok;
}

static auto ui_load_font_face(OxStringView path, bool is_fallback) -> OxStatus {
  ZoneScoped;

  const auto virtual_path = (std::filesystem::path(VFS::ASSETS_DIR) / to_view(path)).generic_string();
  return Rml::LoadFontFace(virtual_path, is_fallback) ? OxStatus::Ok : OxStatus::NotFound;
}

static auto ui_document(u64 document, OxUiDocumentCommand command) -> OxStatus {
  auto* element = element_of(document);
  auto* rml_document = element ? element->GetOwnerDocument() : nullptr;
  if (!rml_document || rml_document != element) {
    return OxStatus::NotFound;
  }

  switch (command) {
    case OxUiDocumentCommand::Show : rml_document->Show(); break;
    case OxUiDocumentCommand::Hide : rml_document->Hide(); break;
    case OxUiDocumentCommand::Close: rml_document->Close(); break;
  }

  return OxStatus::Ok;
}

static auto ui_query(u64 element, OxStringView selector, u64* found) -> OxStatus {
  *found = 0;
  auto* root = element_of(element);
  if (!root) {
    return OxStatus::NotFound;
  }

  *found = handle_of(root->QuerySelector(to_string(selector)));
  return *found ? OxStatus::Ok : OxStatus::NotFound;
}

static auto ui_set_inner_rml(u64 element, OxStringView rml) -> OxStatus {
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  target->SetInnerRML(to_string(rml));
  return OxStatus::Ok;
}

static auto ui_get_inner_rml(u64 element, c8* out, u32 capacity, u32* length) -> OxStatus {
  *length = 0;
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  const auto rml = target->GetInnerRML();
  read_text(to_ox(rml), out, capacity, length);
  return OxStatus::Ok;
}

static auto ui_set_attribute(u64 element, OxStringView name, OxStringView value) -> OxStatus {
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  target->SetAttribute(to_string(name), to_string(value));
  return OxStatus::Ok;
}

static auto ui_get_attribute(u64 element, OxStringView name, c8* out, u32 capacity, u32* length) -> OxStatus {
  *length = 0;
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  const auto value = target->GetAttribute<Rml::String>(to_string(name), "");
  read_text(to_ox(value), out, capacity, length);
  return OxStatus::Ok;
}

static auto ui_set_property(u64 element, OxStringView name, OxStringView value) -> OxStatus {
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  return target->SetProperty(to_string(name), to_string(value)) ? OxStatus::Ok : OxStatus::InvalidArgument;
}

static auto ui_set_class(u64 element, OxStringView name, bool is_active) -> OxStatus {
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  target->SetClass(to_string(name), is_active);
  return OxStatus::Ok;
}

static auto ui_element_command(u64 element, OxUiElementCommand command) -> OxStatus {
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  switch (command) {
    case OxUiElementCommand::Focus         : return target->Focus() ? OxStatus::Ok : OxStatus::InvalidArgument;
    case OxUiElementCommand::Blur          : target->Blur(); return OxStatus::Ok;
    case OxUiElementCommand::Click         : target->Click(); return OxStatus::Ok;
    case OxUiElementCommand::ScrollIntoView: target->ScrollIntoView(true); return OxStatus::Ok;
    case OxUiElementCommand::Remove        : {
      auto* parent = target->GetParentNode();
      if (!parent || target->GetOwnerDocument() == target) {
        return OxStatus::InvalidArgument;
      }

      auto removed = parent->RemoveChild(target);
      if (managed_depth > 0) {
        graveyard.push_back(std::move(removed));
      }

      return OxStatus::Ok;
    }
  }

  return OxStatus::InvalidArgument;
}

static auto ui_append_child(u64 parent, OxStringView tag, u64* child) -> OxStatus {
  *child = 0;
  auto* target = element_of(parent);
  if (!target) {
    return OxStatus::NotFound;
  }

  auto* document = target->GetOwnerDocument();
  if (!document) {
    return OxStatus::InvalidArgument;
  }

  auto created = document->CreateElement(to_string(tag));
  if (!created) {
    return OxStatus::InvalidArgument;
  }

  *child = handle_of(target->AppendChild(std::move(created)));
  return OxStatus::Ok;
}

static auto ui_query_all(u64 element, OxStringView selector, u64* found, i32 capacity, i32* count) -> OxStatus {
  *count = 0;
  auto* root = element_of(element);
  if (!root) {
    return OxStatus::NotFound;
  }

  auto matches = Rml::ElementList{};
  root->QuerySelectorAll(matches, to_string(selector));
  *count = static_cast<i32>(matches.size());
  const auto written = std::min(matches.size(), static_cast<usize>(std::max(capacity, 0)));
  for (usize i = 0; i < written; i++) {
    found[i] = handle_of(matches[i]);
  }

  return OxStatus::Ok;
}

static auto ui_parent(u64 element, u64* parent) -> OxStatus {
  *parent = 0;
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  // a document's parent is the context's root, which isn't part of any document
  auto* above = target->GetParentNode();
  *parent = above && above->GetOwnerDocument() ? handle_of(above) : 0;
  return OxStatus::Ok;
}

static auto ui_get_property(u64 element, OxStringView name, c8* out, u32 capacity, u32* length) -> OxStatus {
  *length = 0;
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  const auto* property = target->GetProperty(to_string(name));
  if (!property) {
    return OxStatus::InvalidArgument;
  }

  const auto text = property->ToString();
  read_text(to_ox(text), out, capacity, length);
  return OxStatus::Ok;
}

static auto ui_has_class(u64 element, OxStringView name, bool* is_set) -> OxStatus {
  *is_set = false;
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  *is_set = target->IsClassSet(to_string(name));
  return OxStatus::Ok;
}

static auto ui_event_parameter(void* event, OxStringView name, OxValue* value) -> OxStatus {
  static auto text = Rml::String{};

  *value = OxValue{};
  if (!event) {
    return OxStatus::NotFound;
  }

  const auto& parameters = static_cast<Rml::Event*>(event)->GetParameters();
  const auto it = parameters.find(to_string(name));
  if (it == parameters.end()) {
    return OxStatus::NotFound;
  }

  *value = to_value(it->second, text);
  return OxStatus::Ok;
}

static auto ui_event_stop(void* event, bool is_immediate) -> void {
  if (!event) {
    return;
  }

  auto* rml_event = static_cast<Rml::Event*>(event);
  if (is_immediate) {
    rml_event->StopImmediatePropagation();
  } else {
    rml_event->StopPropagation();
  }
}

static auto ui_add_listener(u64 element, OxStringView event, u64* listener) -> OxStatus {
  ZoneScoped;

  *listener = 0;
  auto* target = element_of(element);
  if (!target) {
    return OxStatus::NotFound;
  }

  auto added = std::make_unique<ManagedListener>();
  added->id = next_listener++;
  added->element = target->GetObserverPtr();
  added->event = to_string(event);
  added->context = target->GetContext();

  target->AddEventListener(added->event, added.get());
  *listener = added->id;
  listeners.push_back(std::move(added));
  return OxStatus::Ok;
}

static auto remove_listener(std::vector<std::unique_ptr<ManagedListener>>::iterator it) -> void {
  if (auto* element = (*it)->element.get()) {
    element->RemoveEventListener((*it)->event, it->get());
  }

  listeners.erase(it);
}

static auto ui_remove_listener(u64 listener) -> void {
  const auto it = std::ranges::find(listeners, listener, [](const auto& added) { return added->id; });
  if (it != listeners.end()) {
    remove_listener(it);
  }
}

static auto ui_model_create(ecs_world_t* world, OxStringView name, u64* model) -> OxStatus {
  ZoneScoped;

  *model = 0;
  auto* context = context_of(world);
  if (!context) {
    return OxStatus::NotFound;
  }

  auto created = std::make_unique<ManagedModel>();
  created->id = next_model++;
  created->world = world;
  created->context = context;
  created->name = to_string(name);
  created->constructor = context->CreateDataModel(created->name);
  if (!created->constructor) {
    return OxStatus::InvalidArgument;
  }

  *model = created->id;
  models.push_back(std::move(created));
  return OxStatus::Ok;
}

static auto ui_model_bind(u64 model, OxStringView name, i32 binding, bool is_writable) -> OxStatus {
  auto* bound = model_of(model);
  if (!bound) {
    return OxStatus::NotFound;
  }

  const auto id = bound->id;
  auto getter = [id, binding](Rml::Variant& variant) {
    const auto* managed = managed_api();
    if (!managed) {
      return;
    }

    // a string's text is managed memory that stays pinned only until the next call, so it is copied right away
    const ManagedCall call;
    auto value = OxValue{};
    managed->ui_model_get(id, binding, &value);
    variant = to_variant(value);
  };
  auto setter = Rml::DataSetFunc{};
  if (is_writable) {
    setter = [bound, id, binding](const Rml::Variant& variant) {
      if (const auto* managed = managed_api()) {
        const ManagedCall call;
        const auto value = to_value(variant, bound->text);
        managed->ui_model_set(id, binding, &value);
      }
    };
  }

  return bound->constructor.BindFunc(to_string(name), std::move(getter), std::move(setter)) ? OxStatus::Ok
                                                                                            : OxStatus::InvalidArgument;
}

static auto ui_model_bind_event(u64 model, OxStringView name, i32 binding) -> OxStatus {
  auto* bound = model_of(model);
  if (!bound) {
    return OxStatus::NotFound;
  }

  const auto id = bound->id;
  const auto is_bound = bound->constructor.BindEventCallback(
    to_string(name),
    [id, binding](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& arguments) {
      const auto* managed = managed_api();
      if (!managed) {
        return;
      }

      const ManagedCall call;
      memory::ScopedStack stack;
      auto texts = ankerl::svector<Rml::String, 4>(arguments.size());
      auto values = stack.alloc<OxValue>(arguments.size());
      for (usize i = 0; i < arguments.size(); i++) {
        values[i] = to_value(arguments[i], texts[i]);
      }

      const auto ox_event = to_event(event, event.GetType());
      managed->ui_model_event(id, binding, &ox_event, values.data(), static_cast<i32>(values.size()));
    }
  );
  return is_bound ? OxStatus::Ok : OxStatus::InvalidArgument;
}

static auto ui_model_bind_list(u64 model, OxStringView name, i32 binding, const OxStringView* fields, i32 field_count)
  -> OxStatus {
  auto* bound = model_of(model);
  if (!bound) {
    return OxStatus::NotFound;
  }

  const auto count = std::max(field_count, 0);
  auto item = std::make_unique<ListItemDefinition>(bound->id, binding, count == 0);
  for (i32 i = 0; i < count; i++) {
    auto field = std::make_unique<ListFieldDefinition>(bound->id, binding, i);
    item->names.push_back(to_string(fields[i]));
    item->fields.push_back(field.get());
    bound->definitions.push_back(std::move(field));
  }

  auto list = std::make_unique<ListDefinition>(bound->id, binding, item.get());
  const auto is_bound = bound->constructor.BindCustomDataVariable(
    to_string(name),
    Rml::DataVariable(list.get(), nullptr)
  );
  bound->definitions.push_back(std::move(item));
  bound->definitions.push_back(std::move(list));
  return is_bound ? OxStatus::Ok : OxStatus::InvalidArgument;
}

static auto ui_model_dirty(u64 model, OxStringView name) -> void {
  auto* bound = model_of(model);
  if (!bound) {
    return;
  }

  auto handle = bound->constructor.GetModelHandle();
  if (name.length == 0) {
    handle.DirtyAllVariables();
  } else {
    handle.DirtyVariable(to_string(name));
  }
}

static auto remove_model(std::vector<std::unique_ptr<ManagedModel>>::iterator it) -> void {
  (*it)->context->RemoveDataModel((*it)->name);
  models.erase(it);
}

static auto ui_model_remove(u64 model) -> void {
  const auto it = std::ranges::find(models, model, [](const auto& bound) { return bound->id; });
  if (it != models.end()) {
    remove_model(it);
  }
}

auto set_ui_context(ecs_world_t* world, Rml::Context* context) -> void {
  if (context) {
    contexts[world] = context;
  } else {
    contexts.erase(world);
  }
}

auto release_ui(ecs_world_t* world) -> void {
  ZoneScoped;

  auto* context = context_of(world);
  for (auto it = listeners.begin(); it != listeners.end();) {
    if (!context || (*it)->context != context) {
      ++it;
      continue;
    }

    if (auto* element = (*it)->element.get()) {
      element->RemoveEventListener((*it)->event, it->get());
    }

    it = listeners.erase(it);
  }

  for (auto it = models.begin(); it != models.end();) {
    if ((*it)->world != world) {
      ++it;
      continue;
    }

    (*it)->context->RemoveDataModel((*it)->name);
    it = models.erase(it);
  }

  if (context) {
    sweep_elements(context);
  }
}

auto release_ui() -> void {
  ZoneScoped;

  while (!listeners.empty()) {
    remove_listener(listeners.begin());
  }

  while (!models.empty()) {
    remove_model(models.begin());
  }

  elements.clear();
  element_handles.clear();
  swept_elements = 0;
  graveyard.clear();
}

static auto ui_clear_style_sheet_cache() -> void { Rml::Factory::ClearStyleSheetCache(); }

static auto ui_clear_template_cache() -> void { Rml::Factory::ClearTemplateCache(); }

auto fill_ui_api(OxNativeApi& api) -> void {
  api.ui_load_document = ui_load_document;
  api.ui_load_font_face = ui_load_font_face;
  api.ui_document = ui_document;
  api.ui_query = ui_query;
  api.ui_set_inner_rml = ui_set_inner_rml;
  api.ui_get_inner_rml = ui_get_inner_rml;
  api.ui_set_attribute = ui_set_attribute;
  api.ui_get_attribute = ui_get_attribute;
  api.ui_set_property = ui_set_property;
  api.ui_set_class = ui_set_class;
  api.ui_add_listener = ui_add_listener;
  api.ui_remove_listener = ui_remove_listener;
  api.ui_model_create = ui_model_create;
  api.ui_model_bind = ui_model_bind;
  api.ui_model_bind_event = ui_model_bind_event;
  api.ui_model_dirty = ui_model_dirty;
  api.ui_model_remove = ui_model_remove;
  api.ui_clear_style_sheet_cache = ui_clear_style_sheet_cache;
  api.ui_clear_template_cache = ui_clear_template_cache;
  api.ui_element_command = ui_element_command;
  api.ui_append_child = ui_append_child;
  api.ui_query_all = ui_query_all;
  api.ui_parent = ui_parent;
  api.ui_get_property = ui_get_property;
  api.ui_has_class = ui_has_class;
  api.ui_event_parameter = ui_event_parameter;
  api.ui_event_stop = ui_event_stop;
  api.ui_model_bind_list = ui_model_bind_list;
}
} // namespace ox
