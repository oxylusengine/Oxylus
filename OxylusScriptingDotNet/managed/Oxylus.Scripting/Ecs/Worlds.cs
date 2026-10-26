using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;

namespace Oxylus;

// what a world needs on the managed side: its component ids, by ComponentType<T>.Index, the systems the scripts
// registered in it, and a scene's scripts and behaviours
internal sealed class WorldState(nint handle, ulong[] componentIds)
{
    public readonly nint Handle = handle;
    public readonly ulong[] ComponentIds = componentIds;
    public readonly List<ulong> Systems = [];

    public readonly List<SceneBehaviour> Scripts = [];
    public readonly Dictionary<ulong, EntityBehaviour> Behaviours = [];
    public readonly BehaviourList AllBehaviours = new(BehaviourList.All);
    public readonly BehaviourList Updating = new(BehaviourList.Update);
    public readonly BehaviourList FixedUpdating = new(BehaviourList.FixedUpdate);
    public readonly List<EntityBehaviour> Starting = [];
    public bool IsRunning;

    // what Defer queued, run at the start of the next update
    public List<Action> Deferred = [];
    public List<Action> Running = [];
}

internal static class Worlds
{
    // a scene's edit world and play world at most, a scan beats hashing
    static readonly List<WorldState> attached = [];
    static WorldState? last;

    public static void Add(WorldState world) => attached.Add(world);

    public static WorldState? Remove(nint handle)
    {
        var index = attached.FindIndex(world => world.Handle == handle);
        if (index < 0)
        {
            return null;
        }

        var world = attached[index];
        attached.RemoveAt(index);
        last = null;
        return world;
    }

    public static List<WorldState> All => attached;

    public static WorldState? TryGet(nint handle)
    {
        var cached = last;
        if (cached is not null && cached.Handle == handle)
        {
            return cached;
        }

        foreach (var world in attached)
        {
            if (world.Handle == handle)
            {
                last = world;
                return world;
            }
        }

        return null;
    }

    public static void Clear()
    {
        attached.Clear();
        last = null;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static WorldState Get(nint handle)
    {
        var cached = last;
        return cached is not null && cached.Handle == handle ? cached : Find(handle);
    }

    static WorldState Find(nint handle)
    {
        var world = TryGet(handle);
        if (world is null)
        {
            ThrowNotAttached();
        }

        return world;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ulong IdOf<T>(nint handle) where T : unmanaged
    {
        var ids = Get(handle).ComponentIds;
        var index = ComponentType<T>.Index;
        if ((uint)index >= (uint)ids.Length || ids[index] == 0)
        {
            ThrowUnknownComponent<T>();
        }

        return ids[index];
    }

    [DoesNotReturn]
    static void ThrowNotAttached() =>
        throw new InvalidOperationException("This world isn't attached to the scripts, the engine attaches a scene's world when it loads");

    [DoesNotReturn]
    static void ThrowUnknownComponent<T>() =>
        throw new InvalidOperationException($"{typeof(T).Name} isn't a component in this world, mark it [Component] or import its module");
}
