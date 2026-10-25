using System;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public readonly unsafe struct World(nint handle) : IEquatable<World>
{
    public nint Handle { get; } = handle;

    public Entity CreateEntity() => new(Handle, Runtime.Native.EcsNew(Handle));

    public Entity CreateEntity(string name) => CreateEntity().SetName(name);

    // a "." separated path, "Parent.Child", or the default entity when nothing has it
    public Entity Lookup(string path)
    {
        var bytes = Encoding.UTF8.GetBytes(path);
        fixed (byte* data = bytes)
        {
            return new Entity(Handle, Runtime.Native.Lookup(Handle, new StringView(data, bytes.Length)));
        }
    }

    // commands hold back until the matching End, the way a system's do. They nest: only the outermost End applies them
    public void DeferBegin() => Runtime.Native.EcsDeferBegin(Handle);

    public void DeferEnd() => Runtime.Native.EcsDeferEnd(Handle);

    // lets commands apply immediately inside a defer, until DeferResume
    public void DeferSuspend() => Runtime.Native.EcsDeferSuspend(Handle);

    public void DeferResume() => Runtime.Native.EcsDeferResume(Handle);

    public Query<T1> Query<T1>()
        where T1 : unmanaged => new(this);

    public Query<T1, T2> Query<T1, T2>()
        where T1 : unmanaged
        where T2 : unmanaged => new(this);

    public Query<T1, T2, T3> Query<T1, T2, T3>()
        where T1 : unmanaged
        where T2 : unmanaged
        where T3 : unmanaged => new(this);

    public Query<T1, T2, T3, T4> Query<T1, T2, T3, T4>()
        where T1 : unmanaged
        where T2 : unmanaged
        where T3 : unmanaged
        where T4 : unmanaged => new(this);

    internal ulong IdOf<T>() where T : unmanaged => Worlds.IdOf<T>(Handle);

    public bool Equals(World other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is World other && Equals(other);
    public override int GetHashCode() => Handle.GetHashCode();

    public static bool operator ==(World left, World right) => left.Equals(right);
    public static bool operator !=(World left, World right) => !left.Equals(right);
}
