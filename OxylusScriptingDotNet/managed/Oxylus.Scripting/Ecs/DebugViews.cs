using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;

using Oxylus.Hosting;

namespace Oxylus;

// what a debugger shows for an Entity. It runs while the process is stopped, so nothing here may reach native code
// for a world the scripts don't have attached, the handle can outlive its scene
internal sealed unsafe class EntityDebugView(Entity entity)
{
    const int MaxChildren = 256;

    public string Status => entity.DebuggerStatus;
    public ulong Id => entity.Id;
    public World World => entity.World;

    public string? Name => IsAlive ? Read(() => entity.Name, null) : null;
    public string? Path => IsAlive ? Read(() => entity.Path, null) : null;

    public Entity? Parent => IsAlive ? Read(ReadParent, null) : null;

    public Entity[] Children => IsAlive ? Read(ReadChildren, []) : [];

    public ComponentView[] Components => IsAlive ? Read(ReadComponents, []) : [];

    public EntityBehaviour? Behaviour =>
        IsAlive ? Read(() => Worlds.TryGet(entity.World.Handle)?.Behaviours.GetValueOrDefault(entity.Id), null) : null;

    bool IsAlive => entity.DebuggerStatus == "alive";

    Entity? ReadParent()
    {
        var parent = entity.Parent;
        return parent.Id == 0 ? null : parent;
    }

    Entity[] ReadChildren()
    {
        var children = new List<Entity>();
        foreach (var child in entity.Children)
        {
            if (children.Count == MaxChildren)
            {
                break;
            }

            children.Add(child);
        }

        return [.. children];
    }

    // the ones with a C# struct first, with their values, then by flecs path whatever has none
    ComponentView[] ReadComponents()
    {
        var world = entity.World.Handle;
        var views = new List<ComponentView>();
        var readPaths = new HashSet<string>(StringComparer.Ordinal);
        var ids = Worlds.TryGet(world)?.ComponentIds ?? [];
        for (var i = 0; i < ids.Length && i < ComponentType.ReaderCount; i++)
        {
            if (ids[i] == 0 || ComponentType.ReaderAt(i) is not { } reader)
            {
                continue;
            }

            try
            {
                var read = reader.Read(entity);
                if (!read.IsPresent)
                {
                    continue;
                }

                views.Add(new ComponentView(reader.Name, read.Value, read.Value is null ? ComponentKind.Tag : ComponentKind.Value));
                readPaths.Add(new Entity(world, ids[i]).Path);
            }
            catch
            {
            }
        }

        foreach (var part in TypeString().Split(", "))
        {
            var component = part.Trim();
            var isNameOrRead = component.StartsWith("(Identifier,", StringComparison.Ordinal) || readPaths.Contains(component);
            if (component.Length == 0 || isNameOrRead)
            {
                continue;
            }

            var kind = component.StartsWith('(') ? ComponentKind.Pair : ComponentKind.NoStruct;
            views.Add(new ComponentView(component, null, kind));
        }

        return [.. views];
    }

    string TypeString()
    {
        var world = entity.World.Handle;
        Span<byte> buffer = stackalloc byte[512];
        uint length;
        fixed (byte* data = buffer)
        {
            length = Runtime.Native.GetTypeStr(world, entity.Id, data, (uint)buffer.Length);
        }

        if (length <= buffer.Length)
        {
            return Encoding.UTF8.GetString(buffer[..(int)length]);
        }

        var large = new byte[length];
        fixed (byte* data = large)
        {
            Runtime.Native.GetTypeStr(world, entity.Id, data, length);
        }

        return Encoding.UTF8.GetString(large);
    }

    static T Read<T>(Func<T> read, T fallback)
    {
        try
        {
            return read();
        }
        catch
        {
            return fallback;
        }
    }
}

internal enum ComponentKind
{
    Value,
    Tag,
    Pair,
    NoStruct,
}

// the name goes in the display text, Rider ignores DebuggerDisplay's Name
[DebuggerDisplay("{Display,nq}")]
internal sealed class ComponentView(string name, object? value, ComponentKind kind)
{
    [DebuggerBrowsable(DebuggerBrowsableState.Never)]
    public string Name => name;

    [DebuggerBrowsable(DebuggerBrowsableState.Never)]
    public ComponentKind Kind => kind;

    [DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
    public object? Value => value;

    [DebuggerBrowsable(DebuggerBrowsableState.Never)]
    public string Display
    {
        get
        {
            switch (kind)
            {
                case ComponentKind.Tag:
                    return $"{name} (tag)";
                case ComponentKind.Pair:
                    return name;
                case ComponentKind.NoStruct:
                    return $"{name} (no C# struct)";
            }

            try
            {
                // a struct without its own ToString only repeats its type name
                var text = value?.ToString();
                return text is null || text == value!.GetType().ToString() ? name : $"{name} {text}";
            }
            catch
            {
                return name;
            }
        }
    }
}

internal sealed class WorldDebugView(World world)
{
    public nint Handle => world.Handle;
    public bool IsAttached => Read(() => Worlds.TryGet(world.Handle) is not null, false);
    public bool IsRunning => Read(() => Worlds.TryGet(world.Handle)?.IsRunning ?? false, false);
    public SceneBehaviour[] Scripts => Read(() => Worlds.TryGet(world.Handle)?.Scripts.ToArray() ?? [], []);

    public EntityBehaviour[] Behaviours =>
        Read(() => Worlds.TryGet(world.Handle)?.Behaviours.Values.ToArray() ?? [], []);

    static T Read<T>(Func<T> read, T fallback)
    {
        try
        {
            return read();
        }
        catch
        {
            return fallback;
        }
    }
}
