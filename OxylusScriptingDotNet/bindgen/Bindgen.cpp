// writes Components.g.cs, the C# mirrors of every component and enum in CoreComponentsModule. The layouts come from
// flecs meta in this very build, so they carry whatever this platform and build mode do to std::string and UUID

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <flecs.h>
#include <fmt/format.h>
#include <fstream>
#include <glm/ext/quaternion_float.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "Scene/Components.hpp"

namespace ox {
struct CSharpType {
  std::string name = {};
  bool is_component = false;
};

struct Member {
  std::string name = {};
  std::string csharp_name = {};
  std::string type = {};
  u32 offset = 0;
  u32 size = 0;
};

struct Mirror {
  std::string name = {};
  std::string path = {};
  u32 size = 0;
  u32 alignment = 0;
  std::vector<Member> members = {};
  std::vector<std::string> skipped = {};
};

static auto pascal_case(std::string_view name) -> std::string {
  auto result = std::string{};
  auto is_word_start = true;
  for (const auto c : name) {
    if (c == '_') {
      is_word_start = true;
      continue;
    }

    result += is_word_start ? static_cast<c8>(std::toupper(static_cast<unsigned char>(c))) : c;
    is_word_start = false;
  }

  return result;
}

static auto type_size(const flecs::world& world, flecs::entity_t type) -> u32 {
  const auto* info = static_cast<const EcsComponent*>(ecs_get_id(world, type, ecs_id(EcsComponent)));
  return info ? static_cast<u32>(info->size) : 0;
}

static auto write_enum(
  std::ostringstream& out, const flecs::world& world, flecs::entity e, const std::string& underlying
) -> void {
  const auto* constants = static_cast<const EcsConstants*>(ecs_get_id(world, e, ecs_id(EcsConstants)));
  const auto is_unsigned = underlying == "byte" || underlying == "ushort" || underlying == "uint" ||
                           underlying == "ulong";

  out << fmt::format("public enum {} : {}\n{{\n", e.name().c_str(), underlying);
  if (constants) {
    const auto* first = ecs_vec_first_t(&constants->ordered_constants, ecs_enum_constant_t);
    for (const auto& constant : std::span(first, static_cast<usize>(constants->ordered_constants.count))) {
      // flecs keeps an unsigned enum's values apart from the signed ones
      if (is_unsigned) {
        out << fmt::format("    {} = {},\n", pascal_case(constant.name), constant.value_unsigned);
      } else {
        out << fmt::format("    {} = {},\n", pascal_case(constant.name), constant.value);
      }
    }
  }

  out << "}\n\n";
}

static auto write_mirror(std::ostringstream& out, const Mirror& mirror) -> void {
  if (mirror.size == 0) {
    out << fmt::format("public struct {};\n\n", mirror.name);
    return;
  }

  out << fmt::format("[StructLayout(LayoutKind.Explicit, Size = {})]\n", mirror.size);
  out << fmt::format("public struct {}\n{{\n", mirror.name);
  for (const auto& skipped : mirror.skipped) {
    out << fmt::format("    // TODO: {} has no C# mirror yet\n", skipped);
  }

  for (const auto& member : mirror.members) {
    out << fmt::format("    [FieldOffset({})] public {} {};\n", member.offset, member.type, member.csharp_name);
  }

  out << "}\n\n";
}

static auto write_registry(std::ostringstream& out, std::span<const Mirror> mirrors) -> void {
  out << "namespace Oxylus.Interop\n{\n";
  out << "    internal static class CoreComponents\n    {\n";
  out << fmt::format("        internal const int Count = {};\n\n", mirrors.size());
  out << "        internal static void AssignIndices()\n        {\n";
  for (usize i = 0; i < mirrors.size(); i++) {
    out << fmt::format("            ComponentType<global::Oxylus.{}>.Index = {};\n", mirrors[i].name, i);
  }

  out << "        }\n\n";
  out << "        internal static ComponentDescriptor[] CreateDescriptors() =>\n        [\n";
  for (const auto& mirror : mirrors) {
    out << fmt::format(
      "            new(\"{}\"u8.ToArray(), {}, {},\n            [\n",
      mirror.path,
      mirror.size,
      mirror.alignment
    );
    for (const auto& member : mirror.members) {
      out << fmt::format("                new(\"{}\"u8.ToArray(), {}, {}),\n", member.name, member.offset, member.size);
    }

    out << "            ]),\n";
  }

  out << "        ];\n    }\n}\n";
}

static auto generate(const flecs::world& world, flecs::entity core) -> std::string {
  auto types = ankerl::unordered_dense::map<flecs::entity_t, CSharpType>{};
  auto add_type = [&types](flecs::entity_t id, std::string name) {
    types.emplace(id, CSharpType{.name = std::move(name)});
  };

  add_type(world.component<bool>(), "bool");
  add_type(world.component<u8>(), "byte");
  add_type(world.component<u16>(), "ushort");
  add_type(world.component<u32>(), "uint");
  add_type(world.component<u64>(), "ulong");
  add_type(world.component<i8>(), "sbyte");
  add_type(world.component<i16>(), "short");
  add_type(world.component<i32>(), "int");
  add_type(world.component<i64>(), "long");
  add_type(world.component<f32>(), "float");
  add_type(world.component<f64>(), "double");
  add_type(world.component<glm::vec2>(), "global::System.Numerics.Vector2");
  add_type(world.component<glm::vec3>(), "global::System.Numerics.Vector3");
  add_type(world.component<glm::vec4>(), "global::System.Numerics.Vector4");
  add_type(world.component<glm::quat>(), "global::System.Numerics.Quaternion");
  // glm's column major storage read as a row-vector Matrix4x4 is the same transform, so nothing is transposed
  add_type(world.component<glm::mat4>(), "global::System.Numerics.Matrix4x4");
  add_type(world.component<glm::ivec2>(), "global::Oxylus.Int2");
  add_type(world.component<glm::uvec3>(), "global::Oxylus.UInt3");
  add_type(world.component<glm::mat3>(), "global::Oxylus.Matrix3x3");

  auto children = std::vector<flecs::entity>{};
  core.children([&children](flecs::entity child) { children.push_back(child); });
  std::ranges::sort(children, {}, [](flecs::entity e) { return std::string(e.name().c_str()); });

  auto enums = std::ostringstream{};
  for (const auto& child : children) {
    if (const auto* info = static_cast<const EcsEnum*>(ecs_get_id(world, child, ecs_id(EcsEnum)))) {
      const auto underlying = types.find(info->underlying_type);
      write_enum(enums, world, child, underlying != types.end() ? underlying->second.name : "int");
      add_type(child, fmt::format("global::Oxylus.{}", child.name().c_str()));
    }
  }

  auto mirrors = std::vector<Mirror>{};
  for (const auto& child : children) {
    const auto* info = static_cast<const EcsComponent*>(ecs_get_id(world, child, ecs_id(EcsComponent)));
    if (!info || ecs_has_id(world, child, ecs_id(EcsEnum))) {
      continue;
    }

    auto mirror = Mirror{
      .name = child.name().c_str(),
      // the form ecs_lookup takes, "." separated and no root prefix
      .path = child.path(".", "").c_str(),
      .size = static_cast<u32>(info->size),
      .alignment = static_cast<u32>(info->alignment),
    };

    if (const auto* members = static_cast<const EcsStruct*>(ecs_get_id(world, child, ecs_id(EcsStruct)))) {
      const auto* first = ecs_vec_first_t(&members->members, ecs_member_t);
      for (const auto& member : std::span(first, static_cast<usize>(members->members.count))) {
        const auto type = types.find(member.type);
        if (type == types.end() || member.count > 1) {
          mirror.skipped.emplace_back(
            fmt::format("{} ({})", member.name, flecs::entity(world, member.type).name().c_str())
          );
          continue;
        }

        mirror.members.push_back(
          Member{
            .name = member.name,
            .csharp_name = pascal_case(member.name),
            .type = type->second.name,
            .offset = static_cast<u32>(member.offset),
            .size = type_size(world, member.type),
          }
        );
      }
    }

    mirrors.push_back(std::move(mirror));
  }

  auto out = std::ostringstream{};
  out << "// <auto-generated/> by OxCSharpBindgen from the engine's flecs meta, regenerated every build\n";
  out << "#pragma warning disable CS0649\n\n";
  out << "using System.Runtime.InteropServices;\n\n";
  out << "using Oxylus.Interop;\n\n";
  out << "namespace Oxylus\n{\n";
  out << enums.str();
  for (const auto& mirror : mirrors) {
    write_mirror(out, mirror);
  }

  out << "}\n\n";
  write_registry(out, mirrors);
  return out.str();
}
} // namespace ox

auto main(int argc, char** argv) -> int {
  if (argc != 2) {
    std::fprintf(stderr, "usage: OxCSharpBindgen <output .cs file>\n");
    return 1;
  }

  auto world = flecs::world();
  const auto core = world.import<ox::CoreComponentsModule>();

  const auto source = ox::generate(world, core);
  if (source.empty()) {
    return 1;
  }

  const auto output = std::filesystem::path(argv[1]);
  // left untouched when nothing changed, so the C# build has nothing to redo
  if (auto existing = std::ifstream(output, std::ios::binary)) {
    const auto current = std::string(std::istreambuf_iterator<char>(existing), {});
    if (current == source) {
      return 0;
    }
  }

  std::filesystem::create_directories(output.parent_path());
  auto file = std::ofstream(output, std::ios::binary);
  file << source;
  return file ? 0 : 1;
}
