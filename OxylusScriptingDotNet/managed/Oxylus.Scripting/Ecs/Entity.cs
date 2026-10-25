using System;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// the children of one entity, for foreach
public readonly ref struct EntityChildren(nint world, ulong parent)
{
    public ChildEnumerator GetEnumerator() => new(world, parent);
}

public unsafe ref struct ChildEnumerator
{
    readonly nint world;
    void* iter;
    ulong* entities;
    int count;
    int index;

    internal ChildEnumerator(nint world, ulong parent)
    {
        this.world = world;
        iter = NativeMemory.AlignedAlloc(Runtime.Native.IterSize, 16);
        Runtime.Native.ChildrenIter(world, parent, iter);
        entities = null;
        count = 0;
        index = -1;
    }

    public readonly Entity Current => new(world, entities[index]);

    public bool MoveNext()
    {
        if (iter is null)
        {
            return false;
        }

        while (++index >= count)
        {
            ulong* next;
            int nextCount;
            if (!Runtime.Native.ChildrenNext(iter, &next, &nextCount))
            {
                // flecs finishes the iterator when it runs out
                NativeMemory.AlignedFree(iter);
                iter = null;
                return false;
            }

            entities = next;
            count = nextCount;
            index = -1;
        }

        return true;
    }

    public void Dispose()
    {
        if (iter is null)
        {
            return;
        }

        Runtime.Native.IterFini(iter);
        NativeMemory.AlignedFree(iter);
        iter = null;
    }
}

// a flecs entity in one world. Components come back as `ref T` straight into flecs storage: writing through one
// doesn't run OnSet observers, call Modified<T>() after, or use Set<T>() which does both
public readonly unsafe struct Entity(nint world, ulong id) : IEquatable<Entity>
{
    readonly nint world = world;

    public ulong Id { get; } = id;
    public World World => new(world);

    public bool IsAlive => Id != 0 && Runtime.Native.EcsIsAlive(world, Id);

    public ref T Get<T>() where T : unmanaged
    {
        var component = Runtime.Native.EcsGetMutId(world, Id, Worlds.IdOf<T>(world));
        if (component is null)
        {
            ThrowMissing<T>();
        }

        return ref Unsafe.AsRef<T>(component);
    }

    // a null ref when the entity doesn't have it, check with Unsafe.IsNullRef
    public ref T TryGet<T>() where T : unmanaged =>
        ref Unsafe.AsRef<T>(Runtime.Native.EcsGetMutId(world, Id, Worlds.IdOf<T>(world)));

    public bool Has<T>() where T : unmanaged => Runtime.Native.EcsHasId(world, Id, Worlds.IdOf<T>(world));

    public Entity Add<T>() where T : unmanaged
    {
        Runtime.Native.EcsAddId(world, Id, Worlds.IdOf<T>(world));
        return this;
    }

    // adds the component when the entity lacks it, starting from its defaults, and returns its storage. Works inside
    // a defer, where Get can't see the component yet
    public ref T Ensure<T>() where T : unmanaged =>
        ref Unsafe.AsRef<T>(Runtime.Native.EcsEnsureId(world, Id, Worlds.IdOf<T>(world), (nuint)sizeof(T)));

    public Entity Set<T>(in T value) where T : unmanaged
    {
        var copy = value;
        Runtime.Native.EcsSetId(world, Id, Worlds.IdOf<T>(world), (nuint)sizeof(T), &copy);
        return this;
    }

    public Entity Remove<T>() where T : unmanaged
    {
        Runtime.Native.EcsRemoveId(world, Id, Worlds.IdOf<T>(world));
        return this;
    }

    public void Modified<T>() where T : unmanaged => Runtime.Native.EcsModifiedId(world, Id, Worlds.IdOf<T>(world));

    public Entity AddPair(Entity relationship, Entity target)
    {
        Runtime.Native.EcsAddId(world, Id, Pair(relationship.Id, target.Id));
        return this;
    }

    public Entity AddPair<TRelationship>(Entity target) where TRelationship : unmanaged =>
        AddPair(new Entity(world, Worlds.IdOf<TRelationship>(world)), target);

    public Entity AddPair<TRelationship, TTarget>()
        where TRelationship : unmanaged
        where TTarget : unmanaged =>
        AddPair(new Entity(world, Worlds.IdOf<TRelationship>(world)), new Entity(world, Worlds.IdOf<TTarget>(world)));

    public Entity RemovePair(Entity relationship, Entity target)
    {
        Runtime.Native.EcsRemoveId(world, Id, Pair(relationship.Id, target.Id));
        return this;
    }

    public Entity RemovePair<TRelationship>(Entity target) where TRelationship : unmanaged =>
        RemovePair(new Entity(world, Worlds.IdOf<TRelationship>(world)), target);

    public Entity RemovePair<TRelationship, TTarget>()
        where TRelationship : unmanaged
        where TTarget : unmanaged =>
        RemovePair(new Entity(world, Worlds.IdOf<TRelationship>(world)), new Entity(world, Worlds.IdOf<TTarget>(world)));

    public bool HasPair(Entity relationship, Entity target) =>
        Runtime.Native.EcsHasId(world, Id, Pair(relationship.Id, target.Id));

    public bool HasPair<TRelationship>(Entity target) where TRelationship : unmanaged =>
        HasPair(new Entity(world, Worlds.IdOf<TRelationship>(world)), target);

    public bool HasPair<TRelationship, TTarget>()
        where TRelationship : unmanaged
        where TTarget : unmanaged =>
        HasPair(new Entity(world, Worlds.IdOf<TRelationship>(world)), new Entity(world, Worlds.IdOf<TTarget>(world)));

    // the `index`th target of the relationship, or the default entity when there is none
    public Entity Target(Entity relationship, int index = 0) =>
        new(world, Runtime.Native.EcsGetTarget(world, Id, relationship.Id, index));

    public Entity Target<TRelationship>(int index = 0) where TRelationship : unmanaged =>
        Target(new Entity(world, Worlds.IdOf<TRelationship>(world)), index);

    // inherits `prefab`'s components
    public Entity IsA(Entity prefab) => AddPair(new Entity(world, Runtime.Native.IsA), prefab);

    public Entity Parent => new(world, Runtime.Native.EcsGetTarget(world, Id, Runtime.Native.ChildOf, 0));

    public Entity ChildOf(Entity parent)
    {
        Runtime.Native.EcsAddId(world, Id, Pair(Runtime.Native.ChildOf, parent.Id));
        return this;
    }

    // "Parent.Child" from the root, the same path World.Lookup takes
    public string Path
    {
        get
        {
            Span<byte> buffer = stackalloc byte[256];
            uint length;
            fixed (byte* data = buffer)
            {
                length = Runtime.Native.GetPath(world, Id, data, (uint)buffer.Length);
            }

            if (length <= buffer.Length)
            {
                return Encoding.UTF8.GetString(buffer[..(int)length]);
            }

            var large = new byte[length];
            fixed (byte* data = large)
            {
                Runtime.Native.GetPath(world, Id, data, length);
            }

            return Encoding.UTF8.GetString(large);
        }
    }

    // walks the children with no GC allocation. Defer the destruction of a child met on the way, or collect them first
    public EntityChildren Children => new(world, Id);

    public string? Name
    {
        get
        {
            StringView name;
            Runtime.Native.GetName(world, Id, &name);
            return name.Data is null ? null : Encoding.UTF8.GetString(name.Data, (int)name.Length);
        }
    }

    public Entity SetName(string? name)
    {
        if (name is null)
        {
            Runtime.Native.SetName(world, Id, default);
            return this;
        }

        var bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* data = bytes)
        {
            Runtime.Native.SetName(world, Id, new StringView(data, bytes.Length));
        }

        return this;
    }

    public void Destruct() => Runtime.Native.EcsDelete(world, Id);

    // sets the entity's ScriptComponent, so the behaviour is created now if the scene runs or when it starts
    public Entity AddBehaviour<T>() where T : Behaviour
    {
        var name = BehaviourType<T>.Name;
        if (name is null)
        {
            ThrowUnknownBehaviour<T>();
        }

        fixed (byte* data = name)
        {
            Runtime.Native.SetBehaviour(world, Id, new StringView(data, name.Length));
        }

        return this;
    }

    public T? GetBehaviour<T>() where T : Behaviour =>
        Worlds.Get(world).Behaviours.TryGetValue(Id, out var behaviour) ? behaviour as T : null;

    public Entity RemoveBehaviour() => Remove<ScriptComponent>();

    // flecs packs a pair as ECS_PAIR | first << 32 | second, with the generation stripped from second
    static ulong Pair(ulong first, ulong second) => (1UL << 63) | ((first & 0xFFFFFFFF) << 32) | (second & 0xFFFFFFFF);

    public bool Equals(Entity other) => world == other.world && Id == other.Id;
    public override bool Equals(object? obj) => obj is Entity other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(world, Id);
    public override string ToString() => $"Entity({Id})";

    public static bool operator ==(Entity left, Entity right) => left.Equals(right);
    public static bool operator !=(Entity left, Entity right) => !left.Equals(right);

    [DoesNotReturn]
    void ThrowMissing<T>() => throw new InvalidOperationException($"{this} has no {typeof(T).Name}");

    [DoesNotReturn]
    static void ThrowUnknownBehaviour<T>() =>
        throw new InvalidOperationException($"{typeof(T).Name} isn't a behaviour the scripts registered, is it abstract?");
}
