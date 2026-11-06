using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;

using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Oxylus.Generators;

// emits Oxylus.Generated.ScriptRegistry plus the ox_managed_init export a NativeAOT build is entered through, so
// nothing about a scripts assembly is discovered by reflection at runtime
[Generator]
public sealed class ScriptRegistryGenerator : IIncrementalGenerator
{
    static readonly DiagnosticDescriptor InvalidOnLoad = Error(
        "OX0001",
        "Invalid [OnLoad] method",
        "[OnLoad] method '{0}' must be static, parameterless and at least internal");

    static readonly DiagnosticDescriptor InvalidComponentField = Error(
        "OX0002",
        "Unsupported [Component] field",
        "Field '{0}' of [Component] '{1}' is a {2}, components hold numbers, bool, Vector2/3/4 or Quaternion");

    static readonly DiagnosticDescriptor InvalidSystem = Error(
        "OX0003",
        "Invalid [System] method",
        "[System] method '{0}' {1}");

    static readonly DiagnosticDescriptor InvalidScriptType = Error(
        "OX0004",
        "Invalid script class",
        "'{0}' {1}");

    const string SceneBehaviourType = "Oxylus.SceneBehaviour";
    const string EntityBehaviourType = "Oxylus.EntityBehaviour";

    static DiagnosticDescriptor Error(string id, string title, string message) =>
        new(id, title, message, "Oxylus", DiagnosticSeverity.Error, isEnabledByDefault: true);

    static readonly Dictionary<SpecialType, string> PrimitiveFields = new()
    {
        [SpecialType.System_Single] = "F32",
        [SpecialType.System_Double] = "F64",
        [SpecialType.System_SByte] = "I8",
        [SpecialType.System_Int16] = "I16",
        [SpecialType.System_Int32] = "I32",
        [SpecialType.System_Int64] = "I64",
        [SpecialType.System_Byte] = "U8",
        [SpecialType.System_UInt16] = "U16",
        [SpecialType.System_UInt32] = "U32",
        [SpecialType.System_UInt64] = "U64",
        [SpecialType.System_Boolean] = "Bool",
    };

    static readonly Dictionary<string, string> NumericsFields = new()
    {
        ["System.Numerics.Vector2"] = "Vec2",
        ["System.Numerics.Vector3"] = "Vec3",
        ["System.Numerics.Vector4"] = "Vec4",
        ["System.Numerics.Quaternion"] = "Quat",
    };

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var onLoad = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                "Oxylus.OnLoadAttribute",
                static (node, _) => node is MethodDeclarationSyntax,
                static (attributeContext, _) => OnLoadMethod.From((IMethodSymbol)attributeContext.TargetSymbol))
            .Collect();

        var components = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                "Oxylus.ComponentAttribute",
                static (node, _) => node is StructDeclarationSyntax,
                static (attributeContext, _) => ComponentModel.From((INamedTypeSymbol)attributeContext.TargetSymbol))
            .Collect();

        var systems = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                "Oxylus.SystemAttribute",
                static (node, _) => node is MethodDeclarationSyntax,
                static (attributeContext, _) => SystemModel.From(
                    (IMethodSymbol)attributeContext.TargetSymbol,
                    attributeContext.Attributes[0]))
            .Collect();

        // found by what they derive from, like Unity's MonoBehaviour, so a script class needs no attribute
        var scriptTypes = context.SyntaxProvider
            .CreateSyntaxProvider(
                static (node, _) => node is ClassDeclarationSyntax { BaseList: not null },
                static (syntaxContext, _) => syntaxContext.SemanticModel.GetDeclaredSymbol(syntaxContext.Node) is INamedTypeSymbol type
                    ? ScriptTypeModel.From(type)
                    : null)
            .Where(static model => model is not null)
            .Select(static (model, _) => model!)
            .Collect();

        context.RegisterSourceOutput(onLoad.Combine(components).Combine(systems).Combine(scriptTypes), static (spc, input) =>
            Emit(spc, input.Left.Left.Left, input.Left.Left.Right, input.Left.Right, input.Right));
    }

    static void Emit(
        SourceProductionContext context,
        ImmutableArray<OnLoadMethod> onLoad,
        ImmutableArray<ComponentModel> components,
        ImmutableArray<SystemModel> systems,
        ImmutableArray<ScriptTypeModel> scriptTypes)
    {
        foreach (var method in onLoad.Where(m => !m.IsValid))
        {
            context.ReportDiagnostic(Diagnostic.Create(InvalidOnLoad, method.Location, method.Call));
        }

        foreach (var component in components)
        {
            foreach (var field in component.Fields.Where(f => f.FieldType is null))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidComponentField, field.Location, field.Name, component.Name, field.TypeName));
            }
        }

        foreach (var system in systems.Where(s => s.Error is not null))
        {
            context.ReportDiagnostic(Diagnostic.Create(InvalidSystem, system.Location, system.Call, system.Error));
        }

        // a partial class shows up once per declaration
        var distinctScriptTypes = scriptTypes
            .GroupBy(t => t.Type)
            .Select(g => g.First())
            .OrderBy(t => t.Name, System.StringComparer.Ordinal)
            .ToList();
        foreach (var scriptType in distinctScriptTypes.Where(t => t.Error is not null))
        {
            context.ReportDiagnostic(Diagnostic.Create(InvalidScriptType, scriptType.Location, scriptType.Name, scriptType.Error));
        }

        var validComponents = components
            .Where(c => c.Fields.All(f => f.FieldType is not null))
            .OrderBy(c => c.Path, System.StringComparer.Ordinal)
            .ToList();
        var validSystems = systems
            .Where(s => s.Error is null)
            .OrderBy(s => s.Call, System.StringComparer.Ordinal)
            .ToList();

        var source = new StringBuilder();
        source.AppendLine("// <auto-generated/>");
        source.AppendLine("#nullable enable");
        source.AppendLine();
        source.AppendLine("using global::System.Runtime.CompilerServices;");
        source.AppendLine();
        source.AppendLine("namespace Oxylus.Generated;");
        source.AppendLine();
        source.AppendLine("public sealed unsafe class ScriptRegistry : global::Oxylus.Hosting.IScriptRegistry");
        source.AppendLine("{");

        source.AppendLine("    public global::Oxylus.ComponentDescriptor[] RegisterComponents(int firstIndex)");
        source.AppendLine("    {");
        for (var i = 0; i < validComponents.Count; i++)
        {
            source.AppendLine($"        global::Oxylus.ComponentType<{validComponents[i].Type}>.Index = firstIndex + {i};");
        }

        source.AppendLine("        return");
        source.AppendLine("        [");
        for (var i = 0; i < validComponents.Count; i++)
        {
            source.AppendLine($"            DescribeComponent{i}(),");
        }

        source.AppendLine("        ];");
        source.AppendLine("    }");
        source.AppendLine();

        for (var i = 0; i < validComponents.Count; i++)
        {
            EmitComponentDescriptor(source, i, validComponents[i]);
        }

        source.AppendLine("    public global::Oxylus.SystemDescriptor[] CreateSystems() =>");
        source.AppendLine("    [");
        for (var i = 0; i < validSystems.Count; i++)
        {
            if (validSystems[i].Owner is null)
            {
                source.AppendLine($"        {SystemDescriptor(validSystems[i], i)},");
            }
        }

        source.AppendLine("    ];");
        source.AppendLine();

        var validScriptTypes = distinctScriptTypes.Where(t => t.Error is null).ToList();
        source.AppendLine("    public global::Oxylus.ScriptType[] CreateScriptTypes()");
        source.AppendLine("    {");
        for (var i = 0; i < validScriptTypes.Count; i++)
        {
            source.AppendLine($"        var name{i} = \"{validScriptTypes[i].Name}\"u8.ToArray();");
            if (validScriptTypes[i].IsBehaviour)
            {
                source.AppendLine($"        global::Oxylus.BehaviourType<{validScriptTypes[i].Type}>.Name = name{i};");
            }
        }

        source.AppendLine("        return");
        source.AppendLine("        [");
        for (var i = 0; i < validScriptTypes.Count; i++)
        {
            var scriptType = validScriptTypes[i];
            var ownSystems = string.Join(", ", validSystems
                .Select((system, index) => (system, index))
                .Where(s => s.system.Owner == scriptType.Type)
                .Select(s => SystemDescriptor(s.system, s.index)));
            var hooks = scriptType.Hooks.IsEmpty
                ? "global::Oxylus.ScriptHooks.None"
                : string.Join(" | ", scriptType.Hooks.Select(h => $"global::Oxylus.ScriptHooks.{h}"));
            var kind = scriptType.IsBehaviour ? "EntityBehaviour" : "SceneBehaviour";
            source.AppendLine(
                $"            new(name{i}, global::Oxylus.Interop.ScriptKind.{kind}, {hooks}, " +
                $"static () => new {scriptType.Type}(), [{ownSystems}], [{FieldDescriptors(scriptType, i)}]),");
        }

        source.AppendLine("        ];");
        source.AppendLine("    }");
        source.AppendLine();

        for (var i = 0; i < validScriptTypes.Count; i++)
        {
            EmitFieldAccessors(source, i, validScriptTypes[i]);
        }

        for (var i = 0; i < validSystems.Count; i++)
        {
            EmitSystemLoop(source, i, validSystems[i]);
        }

        source.AppendLine("    public void OnLoad()");
        source.AppendLine("    {");
        foreach (var method in onLoad.Where(m => m.IsValid).OrderBy(m => m.Call, System.StringComparer.Ordinal))
        {
            source.AppendLine($"        {method.Call}();");
        }

        source.AppendLine("    }");
        source.AppendLine("}");
        source.AppendLine();
        source.AppendLine("internal static unsafe class ScriptExports");
        source.AppendLine("{");
        source.AppendLine("    [global::System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = \"ox_managed_init\")]");
        source.AppendLine("    public static global::Oxylus.Interop.OxStatus Init(");
        source.AppendLine("        global::Oxylus.Interop.NativeApi* native,");
        source.AppendLine("        global::Oxylus.Interop.ManagedApi* managed,");
        source.AppendLine("        uint abiVersion)");
        source.AppendLine("        => global::Oxylus.Hosting.Runtime.Initialize(native, managed, abiVersion, new ScriptRegistry());");
        source.AppendLine("}");

        context.AddSource("ScriptRegistry.g.cs", source.ToString());
    }

    static string FieldDescriptors(ScriptTypeModel scriptType, int scriptIndex) =>
        string.Join(", ", scriptType.Fields.Select((field, fieldIndex) =>
        {
            var call = $"{AccessorName(scriptIndex, fieldIndex, field)}(({scriptType.Type})s)";
            var read = field.Kind switch
            {
                FieldTextKind.Fixed => $"static _ => \"{Literal(field.Display)}\"",
                FieldTextKind.Value => $"static s => global::Oxylus.Hosting.ScriptFieldText.Value({call})",
                FieldTextKind.Array => $"static s => global::Oxylus.Hosting.ScriptFieldText.Count({call}?.Length, \"{Literal(field.Display)}\")",
                FieldTextKind.Collection => $"static s => global::Oxylus.Hosting.ScriptFieldText.Count((({field.CollectionInterface}?){call})?.Count, \"{Literal(field.Display)}\")",
                _ => $"static s => global::Oxylus.Hosting.ScriptFieldText.Reference({call}, \"{Literal(field.Display)}\")",
            };
            return $"new(\"{Literal(field.Name)}\"u8.ToArray(), {read})";
        }));

    static string AccessorName(int scriptIndex, int fieldIndex, ScriptFieldModel field) =>
        $"ScriptField{scriptIndex}_{fieldIndex}_{field.Name}";

    // generated code can't touch private members, so fields are read through the runtime's field accessors
    static void EmitFieldAccessors(StringBuilder source, int scriptIndex, ScriptTypeModel scriptType)
    {
        for (var i = 0; i < scriptType.Fields.Length; i++)
        {
            var field = scriptType.Fields[i];
            if (field.Kind == FieldTextKind.Fixed)
            {
                continue;
            }

            source.AppendLine($"    [UnsafeAccessor(UnsafeAccessorKind.Field, Name = \"{field.Name}\")]");
            source.AppendLine($"    static extern ref {field.FieldType} {AccessorName(scriptIndex, i, field)}({field.DeclaringType} self);");
            source.AppendLine();
        }
    }

    static string Literal(string text) => text.Replace("\\", "\\\\").Replace("\"", "\\\"");

    static string SystemDescriptor(SystemModel system, int index)
    {
        var terms = string.Join(", ", system.Parameters
            .Where(p => p.Kind == ParameterKind.Component)
            .Select(p => $"new(global::Oxylus.ComponentType<{p.Type}>.Index, global::Oxylus.Interop.TermAccess.{(p.IsIn ? "In" : "InOut")})"));
        return $"new(\"{system.Call.Replace("global::", "")}\"u8.ToArray(), (global::Oxylus.Phase){system.Phase}, [{terms}], &RunSystem{index})";
    }

    // offsets are measured on a real instance, Roslyn doesn't know the layout the runtime picks
    static void EmitComponentDescriptor(StringBuilder source, int index, ComponentModel component)
    {
        source.AppendLine($"    static global::Oxylus.ComponentDescriptor DescribeComponent{index}()");
        source.AppendLine("    {");
        // a fieldless struct is a tag, flecs gives it no storage while C# still sizes it at one byte
        if (component.Fields.IsEmpty)
        {
            source.AppendLine($"        return new(\"{component.Path}\"u8.ToArray(), 0, 0, [], IsScriptDefined: true);");
            source.AppendLine("    }");
            source.AppendLine();
            return;
        }

        source.AppendLine($"        var value = default({component.Type});");
        source.AppendLine($"        ref var start = ref Unsafe.As<{component.Type}, byte>(ref value);");
        source.AppendLine("        return new(");
        source.AppendLine($"            \"{component.Path}\"u8.ToArray(),");
        source.AppendLine($"            (uint)Unsafe.SizeOf<{component.Type}>(),");
        source.AppendLine($"            global::Oxylus.ComponentType.AlignmentOf<{component.Type}>(),");
        source.AppendLine("            [");
        foreach (var field in component.Fields)
        {
            source.AppendLine(
                $"                new(\"{field.Name}\"u8.ToArray(), " +
                $"(uint)Unsafe.ByteOffset(ref start, ref Unsafe.As<{field.TypeName}, byte>(ref value.{field.Name})), " +
                $"(uint)Unsafe.SizeOf<{field.TypeName}>(), global::Oxylus.Interop.ComponentFieldType.{field.FieldType}),");
        }

        source.AppendLine("            ],");
        source.AppendLine("            IsScriptDefined: true,");
        source.AppendLine($"            Defaults: global::Oxylus.ComponentType.DefaultsOf(new {component.Type}()));");
        source.AppendLine("    }");
        source.AppendLine();
    }

    // one call per matched table, the per-entity loop runs here without bounds checks
    static void EmitSystemLoop(StringBuilder source, int index, SystemModel system)
    {
        source.AppendLine("    [global::System.Runtime.InteropServices.UnmanagedCallersOnly]");
        source.AppendLine($"    static void RunSystem{index}(global::Oxylus.Interop.TableView* view)");
        source.AppendLine("    {");
        source.AppendLine("        try");
        source.AppendLine("        {");
        source.AppendLine("            var count = view->Count;");

        var field = 0;
        var arguments = new List<string>();
        foreach (var parameter in system.Parameters)
        {
            switch (parameter.Kind)
            {
                case ParameterKind.Component:
                    source.AppendLine($"            ref var field{field} = ref Unsafe.AsRef<{parameter.Type}>(view->Fields[{field}]);");
                    arguments.Add($"{(parameter.IsIn ? "in" : "ref")} Unsafe.Add(ref field{field}, i)");
                    field++;
                    break;
                case ParameterKind.Entity:
                    arguments.Add("new global::Oxylus.Entity(view->World, view->Entities[i])");
                    break;
                case ParameterKind.DeltaTime:
                    arguments.Add("view->DeltaTime");
                    break;
            }
        }

        source.AppendLine("            for (var i = 0; i < count; i++)");
        source.AppendLine("            {");
        source.AppendLine($"                {system.Call}({string.Join(", ", arguments)});");
        source.AppendLine("            }");
        source.AppendLine("        }");
        source.AppendLine("        catch (global::System.Exception e)");
        source.AppendLine("        {");
        source.AppendLine($"            global::Oxylus.Hosting.Systems.Fail(view, \"{system.Call.Replace("global::", "")}\", e);");
        source.AppendLine("        }");
        source.AppendLine("    }");
        source.AppendLine();
    }

    readonly record struct OnLoadMethod(string Call, bool IsValid, Location? Location)
    {
        public static OnLoadMethod From(IMethodSymbol method)
        {
            var isValid = method.IsStatic
                && method.Parameters.IsEmpty
                && method.TypeParameters.IsEmpty
                && IsAccessible(method);
            return new OnLoadMethod(CallName(method), isValid, method.Locations.FirstOrDefault());
        }
    }

    sealed record FieldModel(string Name, string TypeName, string? FieldType, Location? Location);

    sealed record ComponentModel(string Type, string Name, string Path, ImmutableArray<FieldModel> Fields)
    {
        public static ComponentModel From(INamedTypeSymbol type)
        {
            var fields = type.GetMembers()
                .OfType<IFieldSymbol>()
                .Where(f => !f.IsStatic && !f.IsConst)
                .Select(f => new FieldModel(
                    f.Name,
                    f.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                    FieldTypeOf(f.Type),
                    f.Locations.FirstOrDefault()))
                .ToImmutableArray();

            var path = type.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat);
            return new ComponentModel(
                type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                type.Name,
                path,
                fields);
        }

        static string? FieldTypeOf(ITypeSymbol type) =>
            PrimitiveFields.TryGetValue(type.SpecialType, out var primitive)
                ? primitive
                : NumericsFields.TryGetValue(type.ToDisplayString(), out var numerics) ? numerics : null;
    }

    enum ParameterKind
    {
        Component,
        Entity,
        DeltaTime,
    }

    sealed record ParameterModel(ParameterKind Kind, string Type, bool IsIn);

    sealed record SystemModel(
        string Call,
        int Phase,
        ImmutableArray<ParameterModel> Parameters,
        string? Owner,
        string? Error,
        Location? Location)
    {
        public static SystemModel From(IMethodSymbol method, AttributeData attribute)
        {
            var phase = attribute.ConstructorArguments.Length > 0 && attribute.ConstructorArguments[0].Value is int value
                ? value
                : 3;
            var parameters = ImmutableArray.CreateBuilder<ParameterModel>();
            string? error = null;

            if (!method.IsStatic || !method.TypeParameters.IsEmpty || !IsAccessible(method))
            {
                error = "must be static, non-generic and at least internal";
            }

            foreach (var parameter in method.Parameters)
            {
                var type = parameter.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
                if (type == "global::Oxylus.Entity" && parameter.RefKind == RefKind.None)
                {
                    parameters.Add(new ParameterModel(ParameterKind.Entity, type, false));
                }
                else if (parameter.Type.SpecialType == SpecialType.System_Single && parameter.RefKind == RefKind.None)
                {
                    parameters.Add(new ParameterModel(ParameterKind.DeltaTime, type, false));
                }
                else if (parameter.RefKind is RefKind.Ref or RefKind.In && parameter.Type.IsUnmanagedType && parameter.Type.IsValueType)
                {
                    parameters.Add(new ParameterModel(ParameterKind.Component, type, parameter.RefKind == RefKind.In));
                }
                else
                {
                    error ??= $"has parameter '{parameter.Name}' it can't fill, take components by ref or in, an Entity or a float delta time";
                }
            }

            if (!parameters.Any(p => p.Kind == ParameterKind.Component))
            {
                error ??= "takes no components, so it would match nothing";
            }

            // a scene behaviour's systems register with each instance of it instead of in every world
            var owner = DerivesFrom(method.ContainingType, SceneBehaviourType)
                ? method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                : null;
            return new SystemModel(CallName(method), phase, parameters.ToImmutable(), owner, error, method.Locations.FirstOrDefault());
        }
    }

    enum FieldTextKind
    {
        Fixed,
        Value,
        Array,
        Collection,
        Reference,
    }

    sealed record ScriptFieldModel(
        string Name,
        string Display,
        string DeclaringType,
        string FieldType,
        FieldTextKind Kind,
        string CollectionInterface)
    {
        public static ScriptFieldModel From(IFieldSymbol field, IAssemblySymbol assembly)
        {
            var (kind, collectionInterface) = Classify(field, assembly);
            return new ScriptFieldModel(
                field.Name,
                field.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat),
                field.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                field.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                kind,
                collectionInterface);
        }

        static (FieldTextKind Kind, string CollectionInterface) Classify(IFieldSymbol field, IAssemblySymbol assembly)
        {
            var type = field.Type;
            for (var container = field.ContainingType; container is not null; container = container.ContainingType)
            {
                if (container.IsGenericType)
                {
                    return (FieldTextKind.Fixed, "");
                }
            }

            if (!IsNameable(field.ContainingType, assembly) || !IsNameable(type, assembly))
            {
                return (FieldTextKind.Fixed, "");
            }

            if (type is INamedTypeSymbol { OriginalDefinition.SpecialType: SpecialType.System_Nullable_T } nullable)
            {
                type = nullable.TypeArguments[0];
            }

            if (IsValueText(type))
            {
                return (FieldTextKind.Value, "");
            }

            if (type.TypeKind == TypeKind.Array)
            {
                return (FieldTextKind.Array, "");
            }

            if (type.IsValueType)
            {
                return (FieldTextKind.Fixed, "");
            }

            var isBclCollection = type.TypeKind == TypeKind.Class
                && type.ContainingNamespace?.ToDisplayString().StartsWith("System.Collections", System.StringComparison.Ordinal) == true;
            if (isBclCollection)
            {
                var countInterface = type.AllInterfaces
                    .Where(i => i.OriginalDefinition.ToDisplayString() is "System.Collections.Generic.ICollection<T>" or "System.Collections.Generic.IReadOnlyCollection<T>")
                    .OrderBy(i => i.OriginalDefinition.Name, System.StringComparer.Ordinal)
                    .FirstOrDefault();
                if (countInterface is not null)
                {
                    return (FieldTextKind.Collection, countInterface.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat));
                }
            }

            return (FieldTextKind.Reference, "");
        }

        static bool IsValueText(ITypeSymbol type)
        {
            switch (type.SpecialType)
            {
                case SpecialType.System_Boolean:
                case SpecialType.System_Char:
                case SpecialType.System_SByte:
                case SpecialType.System_Byte:
                case SpecialType.System_Int16:
                case SpecialType.System_UInt16:
                case SpecialType.System_Int32:
                case SpecialType.System_UInt32:
                case SpecialType.System_Int64:
                case SpecialType.System_UInt64:
                case SpecialType.System_Single:
                case SpecialType.System_Double:
                case SpecialType.System_Decimal:
                case SpecialType.System_String:
                    return true;
            }

            return type.TypeKind == TypeKind.Enum
                || type.ToDisplayString() is "System.Numerics.Vector2" or "System.Numerics.Vector3" or "System.Numerics.Vector4"
                    or "System.Numerics.Quaternion" or "Oxylus.Entity";
        }

        static bool IsNameable(ITypeSymbol type, IAssemblySymbol assembly)
        {
            switch (type.TypeKind)
            {
                case TypeKind.Pointer:
                case TypeKind.FunctionPointer:
                case TypeKind.Dynamic:
                case TypeKind.TypeParameter:
                case TypeKind.Error:
                    return false;
                case TypeKind.Array:
                    return IsNameable(((IArrayTypeSymbol)type).ElementType, assembly);
            }

            if (type is not INamedTypeSymbol named)
            {
                return false;
            }

            for (var current = named; current is not null; current = current.ContainingType)
            {
                var isVisible = current.DeclaredAccessibility == Accessibility.Public
                    || (current.DeclaredAccessibility == Accessibility.Internal
                        && SymbolEqualityComparer.Default.Equals(current.ContainingAssembly, assembly));
                if (!isVisible)
                {
                    return false;
                }
            }

            return named.TypeArguments.All(a => IsNameable(a, assembly));
        }
    }

    sealed record ScriptTypeModel(
        string Type,
        string Name,
        bool IsBehaviour,
        ImmutableArray<string> Hooks,
        ImmutableArray<ScriptFieldModel> Fields,
        string? Error,
        Location? Location)
    {
        public static ScriptTypeModel? From(INamedTypeSymbol type)
        {
            var isBehaviour = DerivesFrom(type, EntityBehaviourType);
            if (type.IsAbstract || type.IsStatic || (!isBehaviour && !DerivesFrom(type, SceneBehaviourType)))
            {
                return null;
            }

            string? error = null;
            if (type.IsGenericType)
            {
                error = "is generic, a script class has to name one concrete type";
            }
            else if (!IsReachable(type))
            {
                error = "has to be public or internal, and so does every class it is nested in";
            }
            else if (!type.InstanceConstructors.Any(c => c.Parameters.IsEmpty && c.DeclaredAccessibility is Accessibility.Public or Accessibility.Internal))
            {
                error = "needs a public or internal parameterless constructor";
            }

            var hooks = ImmutableArray.CreateBuilder<string>();
            if (isBehaviour)
            {
                if (Overrides(type, "OnUpdate"))
                {
                    hooks.Add("Update");
                }

                if (Overrides(type, "OnFixedUpdate"))
                {
                    hooks.Add("FixedUpdate");
                }
            }

            var fields = ImmutableArray.CreateBuilder<ScriptFieldModel>();
            if (!isBehaviour && error is null)
            {
                for (var current = type; current is not null && current.ToDisplayString() != SceneBehaviourType; current = current.BaseType)
                {
                    foreach (var field in current.GetMembers().OfType<IFieldSymbol>())
                    {
                        if (!field.IsStatic && !field.IsConst && !field.IsImplicitlyDeclared)
                        {
                            fields.Add(ScriptFieldModel.From(field, type.ContainingAssembly));
                        }
                    }
                }
            }

            return new ScriptTypeModel(
                type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                type.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat),
                isBehaviour,
                hooks.ToImmutable(),
                fields.ToImmutable(),
                error,
                type.Locations.FirstOrDefault());
        }

        static bool IsReachable(INamedTypeSymbol type)
        {
            for (var current = type; current is not null; current = current.ContainingType)
            {
                if (current.DeclaredAccessibility is not (Accessibility.Public or Accessibility.Internal))
                {
                    return false;
                }
            }

            return true;
        }

        // anywhere between the class and Oxylus.EntityBehaviour, a base class can supply the override
        static bool Overrides(INamedTypeSymbol type, string hook)
        {
            for (var current = type; current is not null && current.ToDisplayString() != EntityBehaviourType; current = current.BaseType)
            {
                if (current.GetMembers(hook).OfType<IMethodSymbol>().Any(m => m.IsOverride))
                {
                    return true;
                }
            }

            return false;
        }
    }

    static bool DerivesFrom(INamedTypeSymbol type, string baseName)
    {
        for (var current = type.BaseType; current is not null; current = current.BaseType)
        {
            if (current.ToDisplayString() == baseName)
            {
                return true;
            }
        }

        return false;
    }

    static string CallName(IMethodSymbol method) =>
        $"{method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)}.{method.Name}";

    static bool IsAccessible(IMethodSymbol method) =>
        method.DeclaredAccessibility is Accessibility.Public or Accessibility.Internal;
}
