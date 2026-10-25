using System;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

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

    public Entity Parent => new(world, Runtime.Native.EcsGetTarget(world, Id, Runtime.Native.ChildOf, 0));

    public Entity ChildOf(Entity parent)
    {
        Runtime.Native.EcsAddId(world, Id, Pair(Runtime.Native.ChildOf, parent.Id));
        return this;
    }

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
