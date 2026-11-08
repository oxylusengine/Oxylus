using System;
using System.Collections.Generic;
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

    public string[] Components => IsAlive ? Read(ReadComponents, []) : [];

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

    string[] ReadComponents()
    {
        var world = entity.World.Handle;
        Span<byte> buffer = stackalloc byte[512];
        uint length;
        fixed (byte* data = buffer)
        {
            length = Runtime.Native.GetTypeStr(world, entity.Id, data, (uint)buffer.Length);
        }

        string text;
        if (length <= buffer.Length)
        {
            text = Encoding.UTF8.GetString(buffer[..(int)length]);
        }
        else
        {
            var large = new byte[length];
            fixed (byte* data = large)
            {
                Runtime.Native.GetTypeStr(world, entity.Id, data, length);
            }

            text = Encoding.UTF8.GetString(large);
        }

        var components = new List<string>();
        foreach (var part in text.Split(", "))
        {
            var component = part.Trim();
            if (component.Length == 0 || component.StartsWith("(Identifier,", StringComparison.Ordinal))
            {
                continue;
            }

            components.Add(component);
        }

        return [.. components];
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
