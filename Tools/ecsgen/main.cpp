#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Types.hpp"

namespace ox {
enum class TokenKind : u8 { Identifier, Number, String, Char, Punct, End };

struct Token {
  TokenKind kind = TokenKind::End;
  std::string_view text = {};
  u32 line = 0;
};

struct Attribute {
  std::string key = {};
  std::string value = {};
  u32 line = 0;
};

struct Field {
  std::string name = {};
  std::string type = {};
  std::string asset = {};
  bool transient = false;
  u32 line = 0;
};

struct Enum {
  std::string name = {};
  std::string flecs_name = {};
  bool explicit_bind = false;
  u32 line = 0;
};

struct Component {
  std::string name = {};
  std::string flecs_name = {};
  bool networked = false;
  std::vector<Field> fields = {};
  std::vector<Enum> enums = {};
  u32 line = 0;
};

[[noreturn]]
static auto fail(const std::string_view path, const u32 line, const std::string_view message) -> void {
  std::println(stderr, "{}:{}: error: {}", path, line, message);
  std::exit(1);
}

static auto is_identifier_start(const c8 c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static auto is_digit(const c8 c) -> bool { return c >= '0' && c <= '9'; }

static auto is_identifier_char(const c8 c) -> bool { return is_identifier_start(c) || is_digit(c); }

static auto is_word(const TokenKind kind) -> bool { return kind == TokenKind::Identifier || kind == TokenKind::Number; }

static auto lex(const std::string_view path, const std::string_view source) -> std::vector<Token> {
  auto tokens = std::vector<Token>{};
  auto line = 1_u32;
  auto at_line_start = true;
  auto i = 0_sz;

  const auto peek = [&](const usize offset) -> c8 {
    return i + offset < source.size() ? source[i + offset] : '\0';
  };
  const auto push = [&](const TokenKind kind, const usize begin) {
    tokens.push_back({.kind = kind, .text = source.substr(begin, i - begin), .line = line});
    at_line_start = false;
  };

  while (i < source.size()) {
    const auto c = source[i];
    const auto begin = i;

    if (c == '\n') {
      line += 1;
      at_line_start = true;
      i += 1;
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      i += 1;
    } else if (c == '/' && peek(1) == '/') {
      while (i < source.size() && source[i] != '\n')
        i += 1;
    } else if (c == '/' && peek(1) == '*') {
      const auto start_line = line;
      i += 2;
      while (i < source.size() && !(source[i] == '*' && peek(1) == '/')) {
        line += source[i] == '\n';
        i += 1;
      }
      if (i >= source.size())
        fail(path, start_line, "unterminated block comment");
      i += 2;
    } else if (c == '#' && at_line_start) {
      while (i < source.size() && source[i] != '\n') {
        if (source[i] == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n'))) {
          i += peek(1) == '\r' ? 3 : 2;
          line += 1;
          continue;
        }
        i += 1;
      }
    } else if (is_identifier_start(c)) {
      while (i < source.size() && is_identifier_char(source[i]))
        i += 1;
      push(TokenKind::Identifier, begin);
    } else if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
      i += 1;
      while (i < source.size()) {
        const auto n = source[i];
        const auto p = source[i - 1];
        const auto exponent_sign = (n == '+' || n == '-') && (p == 'e' || p == 'E' || p == 'p' || p == 'P');
        if (!is_identifier_char(n) && n != '.' && n != '\'' && !exponent_sign)
          break;
        i += 1;
      }
      push(TokenKind::Number, begin);
    } else if (c == '"' || c == '\'') {
      i += 1;
      while (i < source.size() && source[i] != c) {
        if (source[i] == '\n')
          fail(path, line, "unterminated literal");
        i += source[i] == '\\' ? 2 : 1;
      }
      if (i >= source.size())
        fail(path, line, "unterminated literal");
      i += 1;
      push(c == '"' ? TokenKind::String : TokenKind::Char, begin);
    } else {
      const auto two = source.substr(i, 2);
      i += (two == "::" || two == "->") ? 2 : 1;
      push(TokenKind::Punct, begin);
    }
  }

  tokens.push_back({.kind = TokenKind::End, .text = {}, .line = line});
  return tokens;
}

static auto closing_of(const std::string_view open) -> c8 {
  if (open == "(")
    return ')';
  if (open == "[")
    return ']';
  if (open == "{")
    return '}';
  return '\0';
}

// index of the token closing the (), [] or {} group opened at `open`, tokens.size() when unbalanced
static auto group_end(std::span<const Token> tokens, const usize open) -> usize {
  auto depth = 0_sz;
  for (auto i = open; i < tokens.size(); i++) {
    const auto text = tokens[i].text;
    if (closing_of(text) != '\0') {
      depth += 1;
    } else if (text == ")" || text == "]" || text == "}") {
      depth -= 1;
      if (depth == 0)
        return i;
    }
  }

  return tokens.size();
}

static auto join_type(std::span<const Token> tokens) -> std::string {
  auto result = std::string{};
  for (usize i = 0; i < tokens.size(); i++) {
    if (i > 0 && is_word(tokens[i - 1].kind) && is_word(tokens[i].kind))
      result += ' ';
    result += tokens[i].text;
  }
  return result;
}

static auto unquote(const std::string_view text) -> std::string {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    return std::string(text.substr(1, text.size() - 2));
  return std::string(text);
}

static auto is_uuid_type(const std::string_view type) -> bool {
  return type == "UUID" || type == "ox::UUID" || type == "::ox::UUID";
}

struct Parser {
  std::string_view path = {};
  std::span<const Token> tokens = {};
  usize pos = 0;
  std::vector<Component> components = {};
  std::string component_namespace = {};

  auto peek(this const Parser& self, const usize offset = 0) -> const Token& {
    return self.tokens[std::min(self.pos + offset, self.tokens.size() - 1)];
  }

  auto is(this const Parser& self, const std::string_view text, const usize offset = 0) -> bool {
    const auto& token = self.peek(offset);
    return token.kind != TokenKind::End && token.text == text;
  }

  auto at_end(this const Parser& self) -> bool { return self.peek().kind == TokenKind::End; }

  auto next(this Parser& self) -> const Token& {
    const auto& token = self.peek();
    if (token.kind != TokenKind::End)
      self.pos += 1;
    return token;
  }

  [[noreturn]]
  auto fail(this const Parser& self, const std::string_view message) -> void {
    ox::fail(self.path, self.peek().line, message);
  }

  [[noreturn]]
  auto fail_at(this const Parser& self, const u32 line, const std::string_view message) -> void {
    ox::fail(self.path, line, message);
  }

  auto describe(this const Parser& self) -> std::string {
    return self.at_end() ? std::string("end of file") : std::format("'{}'", self.peek().text);
  }

  auto expect(this Parser& self, const std::string_view text) -> const Token& {
    if (!self.is(text))
      self.fail(std::format("expected '{}', found {}", text, self.describe()));
    return self.next();
  }

  auto expect_identifier(this Parser& self, const std::string_view what) -> const Token& {
    if (self.peek().kind != TokenKind::Identifier)
      self.fail(std::format("expected {}, found {}", what, self.describe()));
    return self.next();
  }

  // current token opens a (), [] or {} group, moves past its matching close
  auto skip_group(this Parser& self) -> void {
    auto stack = std::string{};
    do {
      if (self.at_end())
        self.fail("unbalanced brackets");
      const auto& token = self.next();
      if (const auto close = closing_of(token.text); close != '\0') {
        stack.push_back(close);
      } else if (token.text == ")" || token.text == "]" || token.text == "}") {
        if (stack.empty() || stack.back() != token.text[0])
          self.fail_at(token.line, std::format("mismatched '{}'", token.text));
        stack.pop_back();
      }
    } while (!stack.empty());
  }

  auto skip_angles(this Parser& self) -> void {
    auto depth = 0_sz;
    do {
      if (self.at_end())
        self.fail("unbalanced '<'");
      if (closing_of(self.peek().text) != '\0') {
        self.skip_group();
        continue;
      }
      const auto& token = self.next();
      if (token.text == "<")
        depth += 1;
      else if (token.text == ">")
        depth -= 1;
    } while (depth > 0);
  }

  // skips a member that isn't an instance field: functions, statics, usings
  auto skip_declaration(this Parser& self) -> void {
    auto seen_parens = false;
    while (true) {
      if (self.at_end())
        self.fail("unterminated declaration");
      if (self.is(";")) {
        self.next();
        return;
      }
      if (self.is("}"))
        self.fail("unexpected '}'");
      if (self.is("(") || self.is("[")) {
        seen_parens |= self.is("(");
        self.skip_group();
        continue;
      }
      if (self.is("{")) {
        self.skip_group();
        if (self.is(";")) {
          self.next();
          return;
        }
        if (seen_parens)
          return;
        continue;
      }
      self.next();
    }
  }

  // `(a, b = c, "str")`, a bare string is a value with no key
  auto parse_attribute_args(this Parser& self) -> std::vector<Attribute> {
    auto attributes = std::vector<Attribute>{};
    self.expect("(");
    while (!self.is(")")) {
      auto attribute = Attribute{.line = self.peek().line};
      if (self.peek().kind == TokenKind::String) {
        attribute.value = unquote(self.next().text);
      } else {
        attribute.key = std::string(self.expect_identifier("an attribute name").text);
        if (self.is("=")) {
          self.next();
          const auto& value = self.next();
          if (value.kind != TokenKind::Identifier && value.kind != TokenKind::String)
            self.fail_at(value.line, std::format("expected a value for '{}'", attribute.key));
          attribute.value = unquote(value.text);
        }
      }
      attributes.push_back(std::move(attribute));
      if (!self.is(","))
        break;
      self.next();
    }
    self.expect(")");
    return attributes;
  }

  auto parse_file(this Parser& self) -> void {
    enum class Scope : u8 { Namespace, Other };
    auto scopes = std::vector<Scope>{};
    auto namespaces = std::vector<std::string>{};

    while (!self.at_end()) {
      if (self.is("namespace")) {
        self.next();
        auto name = std::string{};
        while (self.peek().kind == TokenKind::Identifier || self.is("::"))
          name += self.next().text;
        if (self.is("{")) {
          self.next();
          scopes.push_back(Scope::Namespace);
          namespaces.push_back(std::move(name));
        } else {
          self.skip_declaration();
        }
      } else if (self.is("{")) {
        self.next();
        scopes.push_back(Scope::Other);
      } else if (self.is("}")) {
        if (scopes.empty())
          self.fail("unbalanced '}'");
        if (scopes.back() == Scope::Namespace)
          namespaces.pop_back();
        scopes.pop_back();
        self.next();
      } else if (self.is("OX_COMPONENT")) {
        if (std::ranges::contains(scopes, Scope::Other))
          self.fail("OX_COMPONENT must be at namespace scope");
        auto current_namespace = std::string{};
        for (const auto& name : namespaces)
          current_namespace += current_namespace.empty() ? name : "::" + name;
        self.parse_component(current_namespace);
      } else if (self.is("OX_FIELD") || self.is("OX_TRANSIENT") || self.is("OX_ENUM")) {
        self.fail(std::format("{} outside an OX_COMPONENT struct", self.peek().text));
      } else {
        self.next();
      }
    }

    if (!scopes.empty())
      self.fail("unbalanced '{'");
  }

  auto parse_component(this Parser& self, const std::string_view current_namespace) -> void {
    auto component = Component{.line = self.next().line};

    const auto attributes = self.parse_attribute_args();
    if (!self.is("struct"))
      self.fail("OX_COMPONENT must be followed by 'struct Name {'");
    self.next();
    component.name = std::string(self.expect_identifier("a component name").text);
    if (self.is("final"))
      self.next();
    if (self.is(":"))
      self.fail("components can't have base classes");
    self.expect("{");

    for (const auto& attribute : attributes) {
      if (attribute.key == "networked" && attribute.value.empty())
        component.networked = true;
      else if (attribute.key == "name" && !attribute.value.empty())
        component.flecs_name = attribute.value;
      else
        self.fail_at(attribute.line, std::format("unknown OX_COMPONENT argument '{}'", attribute.key));
    }

    if (self.components.empty())
      self.component_namespace = current_namespace;
    else if (self.component_namespace != current_namespace)
      self.fail_at(component.line, "every component must be declared in the same namespace");

    while (!self.is("}")) {
      if (self.at_end())
        self.fail(std::format("unterminated struct '{}'", component.name));
      self.parse_member(component);
    }
    self.next();
    self.expect(";");

    self.components.push_back(std::move(component));
  }

  auto parse_member(this Parser& self, Component& component) -> void {
    auto transient = false;
    auto asset = std::string{};
    auto field_annotated = false;
    auto enum_annotated = false;
    auto enum_name = std::string{};

    while (true) {
      if (self.is("OX_TRANSIENT")) {
        self.next();
        transient = true;
        field_annotated = true;
      } else if (self.is("OX_FIELD")) {
        self.next();
        for (const auto& attribute : self.parse_attribute_args()) {
          if (attribute.key == "transient" && attribute.value.empty())
            transient = true;
          else if (attribute.key == "asset" && !attribute.value.empty())
            asset = attribute.value;
          else
            self.fail_at(attribute.line, std::format("unknown OX_FIELD argument '{}'", attribute.key));
        }
        field_annotated = true;
      } else if (self.is("OX_ENUM")) {
        const auto line = self.next().line;
        const auto attributes = self.parse_attribute_args();
        if (attributes.size() > 1 || (attributes.size() == 1 && !attributes[0].key.empty()))
          self.fail_at(line, "OX_ENUM takes at most one name string");
        if (!attributes.empty())
          enum_name = attributes[0].value;
        enum_annotated = true;
      } else {
        break;
      }
    }

    if (field_annotated && enum_annotated)
      self.fail("OX_ENUM can't be combined with OX_FIELD or OX_TRANSIENT");

    if (self.is("enum")) {
      if (field_annotated)
        self.fail("OX_FIELD and OX_TRANSIENT annotate fields, not enums");
      self.parse_enum(component, enum_annotated, enum_name);
      return;
    }
    if (enum_annotated)
      self.fail("OX_ENUM must be followed by an enum declaration");

    const auto not_a_field = [&](const std::string_view what) {
      if (field_annotated)
        self.fail(std::format("OX_FIELD and OX_TRANSIENT only apply to instance fields, not {}", what));
    };

    if (self.is(";")) {
      not_a_field("an empty declaration");
      self.next();
      return;
    }

    if (self.is("public") || self.is("private") || self.is("protected")) {
      not_a_field("access specifiers");
      self.next();
      self.expect(":");
      return;
    }

    const auto is_type_key = self.is("struct") || self.is("class") || self.is("union");
    if (is_type_key && self.peek(1).kind == TokenKind::Identifier && self.is(";", 2)) {
      not_a_field("forward declarations");
      self.pos += 3;
      return;
    }
    if (
      is_type_key && self.peek(1).kind == TokenKind::Identifier &&
      (self.is("{", 2) || self.is(":", 2) || self.is("final", 2))
    ) {
      not_a_field("nested types");
      self.pos += 2;
      while (!self.is("{")) {
        if (self.at_end())
          self.fail("unterminated nested type");
        self.next();
      }
      self.skip_group();
      if (!self.is(";"))
        self.fail("declare fields of a nested type separately from the type");
      self.next();
      return;
    }

    if (self.is("template")) {
      not_a_field("templates");
      self.next();
      if (!self.is("<"))
        self.fail("expected '<' after 'template'");
      self.skip_angles();
      self.skip_declaration();
      return;
    }

    for (const auto keyword : {"using", "typedef", "static_assert", "friend"}) {
      if (self.is(keyword)) {
        not_a_field(std::format("'{}' declarations", keyword));
        self.skip_declaration();
        return;
      }
    }

    if (self.is_static_ahead()) {
      not_a_field("static members");
      self.skip_declaration();
      return;
    }

    if (self.is_function_ahead(component)) {
      not_a_field("functions");
      self.skip_declaration();
      return;
    }

    self.parse_fields(component, transient, asset);
  }

  // `inline static` and `constexpr static` put the keyword after other specifiers
  auto is_static_ahead(this const Parser& self) -> bool {
    for (auto i = self.pos; i < self.tokens.size(); i++) {
      const auto& token = self.tokens[i];
      if (token.kind == TokenKind::End || token.text == ";" || token.text == "=" || token.text == "{")
        return false;

      if (token.text == "static" || token.text == "thread_local")
        return true;
    }

    return false;
  }

  // only a `(` at the top level of the declarator opens a parameter list, the ones in `alignas(16)`,
  // `std::array<f32, sizeof(u64)>` or `std::function<void(u32)>` belong to a field
  auto is_function_ahead(this const Parser& self, const Component& component) -> bool {
    auto angle_depth = 0_sz;
    for (auto i = self.pos; i < self.tokens.size(); i++) {
      const auto& token = self.tokens[i];
      if (token.kind == TokenKind::End || token.text == ";" || token.text == "=" || token.text == "{")
        return false;

      if (token.text == "operator")
        return true;

      if (token.text == "<")
        angle_depth += 1;
      else if (token.text == ">" && angle_depth > 0)
        angle_depth -= 1;

      if (closing_of(token.text) == '\0')
        continue;

      const auto previous = i > self.pos ? self.tokens[i - 1].text : std::string_view{};
      const auto is_specifier_group = previous == "alignas" || previous == "decltype" || previous == "__attribute__" ||
                                      previous == "__declspec";
      if (token.text == "(" && angle_depth == 0 && !is_specifier_group) {
        if (previous == component.name)
          self.fail_at(token.line, "components must stay aggregates, they can't declare constructors or destructors");

        if (self.is("*", i - self.pos + 1) || self.is("&", i - self.pos + 1))
          self.fail_at(
            token.line,
            "function pointer fields aren't supported, name the type with an alias outside the component"
          );

        return true;
      }

      i = group_end(self.tokens, i);
    }

    return false;
  }

  auto parse_fields(this Parser& self, Component& component, const bool transient, const std::string_view asset)
    -> void {
    const auto line = self.peek().line;
    // leading attributes stay out of the type, it's compared against enum and UUID spellings
    while (self.is("alignas") || (self.is("[") && self.is("[", 1))) {
      if (self.is("alignas"))
        self.next();
      self.skip_group();
    }

    const auto begin = self.pos;
    auto angle_depth = 0_sz;

    while (true) {
      if (self.at_end())
        self.fail("unterminated field declaration");
      if (self.is("(") || (self.is("[") && self.is("[", 1))) {
        self.skip_group();
        continue;
      }
      if (self.is("["))
        self.fail("array fields aren't supported, wrap the array in a struct");
      if (self.is(":"))
        self.fail("bitfields aren't supported");
      if (self.is("<"))
        angle_depth += 1;
      if (self.is(">") && angle_depth > 0)
        angle_depth -= 1;
      if (self.is("=") || self.is("{") || self.is(";") || (self.is(",") && angle_depth == 0))
        break;
      self.next();
    }

    const auto declarator = self.tokens.subspan(begin, self.pos - begin);
    if (declarator.size() < 2 || declarator.back().kind != TokenKind::Identifier)
      self.fail_at(line, "expected a field declaration");

    const auto type = join_type(declarator.first(declarator.size() - 1));
    auto name = std::string(declarator.back().text);

    while (true) {
      component.fields.push_back({
        .name = std::move(name),
        .type = type,
        .asset = std::string(asset),
        .transient = transient,
        .line = line,
      });

      self.skip_initializer();
      if (self.is(";")) {
        self.next();
        return;
      }

      if (!self.is(","))
        self.fail(
          std::format(
            "unexpected {} in field declaration, a top-level comma in an initializer needs parentheses",
            self.describe()
          )
        );
      self.next();
      if (self.is("*") || self.is("&"))
        self.fail("declare pointer and reference fields on their own line");
      name = std::string(self.expect_identifier("a field name").text);
    }
  }

  auto skip_initializer(this Parser& self) -> void {
    if (self.is("{")) {
      self.skip_group();
      return;
    }
    if (!self.is("="))
      return;

    self.next();
    while (!self.is(",") && !self.is(";")) {
      if (self.at_end() || self.is("}"))
        self.fail("unterminated initializer");
      if (closing_of(self.peek().text) != '\0')
        self.skip_group();
      else
        self.next();
    }
  }

  auto parse_enum(this Parser& self, Component& component, const bool explicit_bind, const std::string_view name)
    -> void {
    const auto line = self.next().line;
    if (self.is("class") || self.is("struct"))
      self.next();

    auto e = Enum{
      .name = std::string(self.expect_identifier("an enum name").text),
      .flecs_name = std::string(name),
      .explicit_bind = explicit_bind,
      .line = line,
    };

    while (!self.is("{")) {
      if (self.at_end() || self.is(";"))
        self.fail("components can't hold opaque enum declarations");
      self.next();
    }
    self.skip_group();
    if (!self.is(";"))
      self.fail("declare fields of an enum type separately from the enum");
    self.next();

    component.enums.push_back(std::move(e));
  }
};

static auto default_enum_name(const Component& component, const Enum& e) -> std::string {
  auto owner = std::string_view(component.name);
  if (owner.ends_with("Component") && owner.size() > std::string_view("Component").size())
    owner.remove_suffix(std::string_view("Component").size());
  return std::format("{}{}", owner, e.name);
}

static auto validate(const std::string_view path, std::span<Component> components) -> void {
  auto flecs_enum_names = std::vector<std::string>{};

  for (auto& component : components) {
    auto serialized = 0_sz;
    for (const auto& field : component.fields) {
      const auto uuid = is_uuid_type(field.type);
      if (!field.asset.empty() && !uuid)
        fail(path, field.line, std::format("'{}' isn't a UUID, OX_FIELD(asset) doesn't apply to it", field.name));
      if (!field.asset.empty() && field.transient)
        fail(path, field.line, std::format("'{}' is transient, OX_FIELD(asset) would never be read", field.name));
      if (uuid && field.asset.empty() && !field.transient)
        fail(path, field.line, std::format("UUID field '{}' needs OX_FIELD(asset = ...) or OX_TRANSIENT", field.name));
      serialized += !field.transient;
    }

    if (serialized == 0)
      fail(path, component.line, std::format("component '{}' has no serialized fields", component.name));

    for (auto& e : component.enums) {
      const auto qualified = std::format("{}::{}", component.name, e.name);
      const auto used = std::ranges::any_of(component.fields, [&](const Field& field) {
        return !field.transient && (field.type == e.name || field.type == qualified);
      });
      e.explicit_bind |= used;
      if (!e.explicit_bind)
        continue;

      if (e.flecs_name.empty())
        e.flecs_name = default_enum_name(component, e);
      if (std::ranges::contains(flecs_enum_names, e.flecs_name))
        fail(
          path,
          e.line,
          std::format("enum name '{}' is already taken, rename it with OX_ENUM(\"...\")", e.flecs_name)
        );
      flecs_enum_names.push_back(e.flecs_name);
    }
  }
}

static auto generate(
  const std::string_view source_name,
  const std::string_view function_name,
  const std::string_view component_namespace,
  std::span<const Component> components
) -> std::string {
  auto out = std::string{};
  const auto emit = [&out]<typename... Args>(std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(out), fmt, std::forward<Args>(args)...);
  };

  const auto any_assets = std::ranges::any_of(components, [](const Component& component) {
    return std::ranges::any_of(component.fields, [](const Field& field) { return !field.asset.empty(); });
  });
  const auto any_networked = std::ranges::any_of(components, [](const Component& component) {
    return component.networked;
  });

  // qualified and self-contained, so a game can include it from any namespace
  emit("// generated by ecsgen from {}, do not edit\n\n", source_name);
  if (any_assets)
    emit("#include <array>\n\n#include \"Asset/AssetFile.hpp\"\n");
  emit("#include \"Scene/ComponentRegistry.hpp\"\n");
  if (any_networked)
    emit("#include \"Scene/Components.hpp\"\n");
  emit("\n");
  if (!component_namespace.empty())
    emit("namespace {} {{\n", component_namespace);
  emit("static auto {}(ox::ComponentRegistry& registry) -> void {{\n", function_name);

  for (const auto& component : components) {
    for (const auto& e : component.enums) {
      if (e.explicit_bind)
        emit("  registry.bind_enum<{}::{}>(\"{}\");\n", component.name, e.name, e.flecs_name);
    }
  }

  for (const auto& component : components) {
    auto members = std::vector<std::string>{};
    for (const auto& field : component.fields) {
      if (!field.transient)
        members.push_back(std::format("&C::{}", field.name));
    }

    emit("\n  {{\n    using C = {};\n", component.name);

    const auto has_assets = std::ranges::any_of(component.fields, [](const Field& field) {
      return !field.asset.empty();
    });
    if (has_assets) {
      emit("    static constexpr auto asset_fields = std::array{{\n");
      for (const auto& field : component.fields) {
        if (!field.asset.empty())
          emit("      ox::AssetField{{\"{}\", ox::AssetType::{}}},\n", field.name, field.asset);
      }
      emit("    }};\n");
    }

    emit("    registry.bind<");
    if (members.size() <= 4) {
      for (usize i = 0; i < members.size(); i++)
        emit("{}{}", i > 0 ? ", " : "", members[i]);
    } else {
      for (usize i = 0; i < members.size(); i++)
        emit("\n      {}{}", members[i], i + 1 < members.size() ? "," : "");
    }
    emit(">(");
    if (!component.flecs_name.empty())
      emit("\"{}\"", component.flecs_name);
    emit(")");

    if (component.networked)
      emit("\n      .tags<ox::Networked>()");
    if (has_assets)
      emit("\n      .asset_fields(asset_fields)");
    emit(";\n  }}\n");
  }

  emit("}}\n");
  if (!component_namespace.empty())
    emit("}} // namespace {}\n", component_namespace);
  return out;
}

static auto read_file(const std::filesystem::path& path) -> std::string {
  auto file = std::ifstream(path, std::ios::binary);
  if (!file)
    return {};
  return std::string(std::istreambuf_iterator<c8>(file), std::istreambuf_iterator<c8>());
}
} // namespace ox

auto main(int argc, char** argv) -> int {
  auto input = std::string_view{};
  auto output = std::string_view{};
  auto function_name = std::string_view{};

  for (auto i = 1; i + 1 < argc; i += 2) {
    const auto flag = std::string_view(argv[i]);
    if (flag == "--input")
      input = argv[i + 1];
    else if (flag == "--output")
      output = argv[i + 1];
    else if (flag == "--function")
      function_name = argv[i + 1];
  }

  if (input.empty() || output.empty() || function_name.empty()) {
    std::println(stderr, "usage: ecsgen --input <header> --function <name> --output <file.inl | ->");
    return 1;
  }

  if (!std::filesystem::is_regular_file(input)) {
    std::println(stderr, "ecsgen: can't read '{}'", input);
    return 1;
  }

  const auto source = ox::read_file(input);
  const auto tokens = ox::lex(input, source);
  auto parser = ox::Parser{.path = input, .tokens = tokens};
  parser.parse_file();

  if (parser.components.empty())
    ox::fail(input, 1, "no OX_COMPONENT structs found");

  ox::validate(input, parser.components);
  const auto source_name = std::filesystem::path(input).filename().string();
  const auto text = ox::generate(source_name, function_name, parser.component_namespace, parser.components);

  if (output == "-") {
    std::print("{}", text);
    return 0;
  }

  const auto output_path = std::filesystem::path(output);
  if (std::filesystem::exists(output_path) && ox::read_file(output_path) == text)
    return 0;

  if (output_path.has_parent_path())
    std::filesystem::create_directories(output_path.parent_path());
  auto file = std::ofstream(output_path, std::ios::binary | std::ios::trunc);
  file << text;
  if (!file) {
    std::println(stderr, "ecsgen: can't write '{}'", output);
    return 1;
  }

  return 0;
}
