using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public readonly struct NetClientId(ulong value) : IEquatable<NetClientId>
{
    public ulong Value { get; } = value;

    public bool Equals(NetClientId other) => Value == other.Value;
    public override bool Equals(object? obj) => obj is NetClientId other && Equals(other);
    public override int GetHashCode() => Value.GetHashCode();
    public override string ToString() => $"NetClient({Value})";

    public static bool operator ==(NetClientId left, NetClientId right) => left.Equals(right);
    public static bool operator !=(NetClientId left, NetClientId right) => !left.Equals(right);
}

// Networking/NetClient.hpp's NetClientStatus
public enum NetClientStatus : uint
{
    None = 0,
    Connecting,
    Connected,
    Disconnected,
    TimedOut,
}

internal unsafe delegate void RpcHandler(ulong sender, Value* args, int count);

// the entities a snapshot changed and removed, in the server's world. Both spans are only valid during the call
public delegate void SceneSnapshotHandler(byte sequence, ReadOnlySpan<ulong> changed, ReadOnlySpan<ulong> removed);

// a server or a client the scripts opened. The engine ticks it every frame, so its events and RPCs arrive on the main
// thread, and it closes on Dispose or when the scripts unload
public abstract unsafe class NetEndpoint : IDisposable
{
    internal static readonly Dictionary<ulong, NetEndpoint> Open = [];

    internal readonly List<RpcHandler> Rpcs = [];
    internal ulong Handle;

    internal NetEndpoint(ulong handle)
    {
        Handle = handle;
        Open[handle] = this;
    }

    public bool IsOpen => Handle != 0;

    public void Dispose()
    {
        if (Handle == 0)
        {
            return;
        }

        Open.Remove(Handle);
        Runtime.Native.NetClose(Handle);
        Handle = 0;
    }

    internal ulong Live => Handle != 0 ? Handle : throw new ObjectDisposedException(GetType().Name);

    internal abstract void OnEvent(NetEvent kind, ulong peer, uint reason);

    internal virtual void OnSnapshot(byte sequence, ReadOnlySpan<ulong> changed, ReadOnlySpan<ulong> removed) { }

    internal static void Release()
    {
        foreach (var endpoint in Open.Values)
        {
            endpoint.Handle = 0;
        }

        Open.Clear();
    }
}

public sealed unsafe class NetServer : NetEndpoint
{
    NetServer(ulong handle) : base(handle) { }

    public event Action<NetClientId>? ClientConnected;
    public event Action<NetClientId>? ClientDisconnected;
    // a client says it has applied the scene snapshot with this sequence
    public event Action<NetClientId, byte>? ClientAcked;

    public static NetServer Start(ushort port, uint maxClients = 32)
    {
        ulong handle;
        if (Runtime.Native.NetServerStart(port, maxClients, &handle) != OxStatus.Ok)
        {
            throw new InvalidOperationException($"Couldn't start a server on port {port}, is it taken?");
        }

        return new NetServer(handle);
    }

    // fills `clients` and returns how many are connected, which can be more than fit
    public int GetClients(Span<NetClientId> clients)
    {
        Span<ulong> ids = stackalloc ulong[clients.Length];
        int count;
        fixed (ulong* data = ids)
        {
            count = Runtime.Native.NetServerClients(Live, data, ids.Length);
        }

        for (var i = 0; i < Math.Min(count, clients.Length); i++)
        {
            clients[i] = new NetClientId(ids[i]);
        }

        return count;
    }

    internal override void OnEvent(NetEvent kind, ulong peer, uint reason)
    {
        switch (kind)
        {
            case NetEvent.ClientConnected:
                ClientConnected?.Invoke(new NetClientId(peer));
                break;
            case NetEvent.ClientDisconnected:
                ClientDisconnected?.Invoke(new NetClientId(peer));
                break;
            case NetEvent.ClientAcked:
                ClientAcked?.Invoke(new NetClientId(peer), (byte)reason);
                break;
        }
    }
}

public sealed unsafe class NetClient : NetEndpoint
{
    NetClient(ulong handle) : base(handle) { }

    public event Action? Connected;
    public event Action<NetClientStatus>? Disconnected;
    public event SceneSnapshotHandler? SceneSnapshot;

    public static NetClient Connect(string host, ushort port, uint timeoutMs = 5000)
    {
        var bytes = Encoding.UTF8.GetBytes(host);
        ulong handle;
        fixed (byte* data = bytes)
        {
            if (Runtime.Native.NetClientConnect(new StringView(data, bytes.Length), port, timeoutMs, &handle) != OxStatus.Ok)
            {
                throw new InvalidOperationException($"Couldn't connect to {host}:{port}");
            }
        }

        return new NetClient(handle);
    }

    public NetClientStatus Status => (NetClientStatus)State.Status;
    // what the server knows this client as, 0 until the handshake finishes
    public ulong NetId => State.NetId;
    public uint Ping => State.Ping;
    public uint Rtt => State.Rtt;

    NetClientState State
    {
        get
        {
            NetClientState state;
            if (Runtime.Native.NetClientState(Live, &state) != OxStatus.Ok)
            {
                throw new ObjectDisposedException(nameof(NetClient));
            }

            return state;
        }
    }

    internal override void OnEvent(NetEvent kind, ulong peer, uint reason)
    {
        switch (kind)
        {
            case NetEvent.Connected:
                Connected?.Invoke();
                break;
            case NetEvent.Disconnected:
                Disconnected?.Invoke((NetClientStatus)reason);
                break;
        }
    }

    internal override void OnSnapshot(byte sequence, ReadOnlySpan<ulong> changed, ReadOnlySpan<ulong> removed) =>
        SceneSnapshot?.Invoke(sequence, changed, removed);
}

// a remote procedure by name. Its parameters are bool, byte, ushort, int, long, float, double, string, AssetId or
// byte[], which travel as the engine's RPC parameters, so a C++ peer reads them the same
public abstract unsafe class RpcDefinition
{
    const ulong Broadcast = ulong.MaxValue;

    readonly byte[] name;

    protected RpcDefinition(string name) => this.name = Encoding.UTF8.GetBytes(name);

    public string Name => Encoding.UTF8.GetString(name);

    internal void Register(NetEndpoint endpoint, RpcHandler handler)
    {
        fixed (byte* data = name)
        {
            if (Runtime.Native.NetRegisterRpc(endpoint.Live, new StringView(data, name.Length), endpoint.Rpcs.Count) != OxStatus.Ok)
            {
                throw new InvalidOperationException($"Couldn't register RPC {Name}");
            }
        }

        endpoint.Rpcs.Add(handler);
    }

    internal void Send(NetEndpoint endpoint, ulong client, ReadOnlySpan<Value> args, bool isReliable)
    {
        OxStatus status;
        fixed (byte* data = name)
        fixed (Value* values = args)
        {
            status = Runtime.Native.NetCall(endpoint.Live, client, new StringView(data, name.Length), values, args.Length, isReliable);
        }

        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException($"Couldn't send RPC {Name}, is the endpoint connected?");
        }
    }

    internal static ulong Target(NetClientId? client) => client?.Value ?? Broadcast;

    internal static void Validate<T>()
    {
        if (typeof(T) != typeof(bool) && typeof(T) != typeof(byte) && typeof(T) != typeof(ushort) &&
            typeof(T) != typeof(int) && typeof(T) != typeof(long) && typeof(T) != typeof(float) &&
            typeof(T) != typeof(double) && typeof(T) != typeof(string) && typeof(T) != typeof(AssetId) &&
            typeof(T) != typeof(byte[]))
        {
            throw new NotSupportedException($"An RPC parameter is a bool, byte, ushort, int, long, float, double, string, AssetId or byte[], not {typeof(T).Name}");
        }
    }

    // what Encode copies into the scratch for a string or a byte array
    internal static int Size<T>(T value) => value switch
    {
        string text => Encoding.UTF8.GetByteCount(text),
        byte[] bytes => bytes.Length,
        AssetId => 16,
        _ => 0,
    };

    internal static Value Encode<T>(T value, ref Scratch scratch)
    {
        var encoded = default(Value);
        if (typeof(T) == typeof(bool))
        {
            encoded.Kind = ValueKind.Bool;
            encoded.Integer = Unsafe.As<T, bool>(ref value) ? 1 : 0;
        }
        else if (typeof(T) == typeof(byte))
        {
            encoded.Kind = ValueKind.U8;
            encoded.Integer = Unsafe.As<T, byte>(ref value);
        }
        else if (typeof(T) == typeof(ushort))
        {
            encoded.Kind = ValueKind.U16;
            encoded.Integer = Unsafe.As<T, ushort>(ref value);
        }
        else if (typeof(T) == typeof(int))
        {
            encoded.Kind = ValueKind.I32;
            encoded.Integer = Unsafe.As<T, int>(ref value);
        }
        else if (typeof(T) == typeof(long))
        {
            encoded.Kind = ValueKind.I64;
            encoded.Integer = Unsafe.As<T, long>(ref value);
        }
        else if (typeof(T) == typeof(float))
        {
            encoded.Kind = ValueKind.F32;
            encoded.Number = Unsafe.As<T, float>(ref value);
        }
        else if (typeof(T) == typeof(double))
        {
            encoded.Kind = ValueKind.F64;
            encoded.Number = Unsafe.As<T, double>(ref value);
        }
        else if (typeof(T) == typeof(AssetId))
        {
            var id = Unsafe.As<T, AssetId>(ref value);
            var data = scratch.Alloc<byte>(16);
            Unsafe.CopyBlockUnaligned(data, &id, 16);
            encoded.Kind = ValueKind.Uuid;
            encoded.Text = new StringView(data, 16);
        }
        else if (typeof(T) == typeof(string))
        {
            var text = (string?)(object?)value ?? "";
            var length = Encoding.UTF8.GetByteCount(text);
            var data = scratch.Alloc<byte>(length);
            Encoding.UTF8.GetBytes(text, new Span<byte>(data, length));
            encoded.Kind = ValueKind.String;
            encoded.Text = new StringView(data, length);
        }
        else
        {
            var bytes = (byte[]?)(object?)value ?? [];
            var data = scratch.Alloc<byte>(bytes.Length);
            bytes.CopyTo(new Span<byte>(data, bytes.Length));
            encoded.Kind = ValueKind.Bytes;
            encoded.Text = new StringView(data, bytes.Length);
        }

        return encoded;
    }

    internal static T Decode<T>(Value* args, int count, int index)
    {
        if (index >= count)
        {
            throw new InvalidOperationException($"The RPC arrived with {count} parameters, {index + 1} were expected");
        }

        ref readonly var value = ref args[index];
        var isNumber = value.Kind is ValueKind.F32 or ValueKind.F64;
        if (typeof(T) == typeof(bool))
        {
            var result = value.Integer != 0;
            return Unsafe.As<bool, T>(ref result);
        }

        if (typeof(T) == typeof(byte))
        {
            var result = (byte)value.Integer;
            return Unsafe.As<byte, T>(ref result);
        }

        if (typeof(T) == typeof(ushort))
        {
            var result = (ushort)value.Integer;
            return Unsafe.As<ushort, T>(ref result);
        }

        if (typeof(T) == typeof(int))
        {
            var result = isNumber ? (int)value.Number : (int)value.Integer;
            return Unsafe.As<int, T>(ref result);
        }

        if (typeof(T) == typeof(long))
        {
            var result = isNumber ? (long)value.Number : value.Integer;
            return Unsafe.As<long, T>(ref result);
        }

        if (typeof(T) == typeof(float))
        {
            var result = isNumber ? (float)value.Number : value.Integer;
            return Unsafe.As<float, T>(ref result);
        }

        if (typeof(T) == typeof(double))
        {
            var result = isNumber ? value.Number : value.Integer;
            return Unsafe.As<double, T>(ref result);
        }

        if (typeof(T) == typeof(AssetId))
        {
            var result = default(AssetId);
            if (value.Text.Length == 16)
            {
                Unsafe.CopyBlockUnaligned(&result, value.Text.Data, 16);
            }

            return Unsafe.As<AssetId, T>(ref result);
        }

        if (typeof(T) == typeof(string))
        {
            return (T)(object)Encoding.UTF8.GetString(value.Text.Data, (int)value.Text.Length);
        }

        return (T)(object)new ReadOnlySpan<byte>(value.Text.Data, (int)value.Text.Length).ToArray();
    }
}

public sealed unsafe class Rpc(string name) : RpcDefinition(name)
{
    public void On(NetServer server, Action<NetClientId> handler) =>
        Register(server, (sender, _, _) => handler(new NetClientId(sender)));

    public void On(NetClient client, Action handler) => Register(client, (_, _, _) => handler());

    public void Send(NetClient client, bool isReliable = true) => Send(client, 0, [], isReliable);

    public void Send(NetServer server, NetClientId? client, bool isReliable = true) => Send(server, Target(client), [], isReliable);
}

public sealed unsafe class Rpc<T1> : RpcDefinition
{
    public Rpc(string name) : base(name) => Validate<T1>();

    public void On(NetServer server, Action<NetClientId, T1> handler) =>
        Register(server, (sender, args, count) => handler(new NetClientId(sender), Decode<T1>(args, count, 0)));

    public void On(NetClient client, Action<T1> handler) =>
        Register(client, (_, args, count) => handler(Decode<T1>(args, count, 0)));

    public void Send(NetClient client, T1 a, bool isReliable = true) => Call(client, 0, a, isReliable);

    // to one client, or to all of them when `client` is null
    public void Send(NetServer server, NetClientId? client, T1 a, bool isReliable = true) => Call(server, Target(client), a, isReliable);

    void Call(NetEndpoint endpoint, ulong client, T1 a, bool isReliable)
    {
        var scratch = new Scratch(Scratch.SizeFor(Size(a), 1));
        try
        {
            Span<Value> args = [Encode(a, ref scratch)];
            Send(endpoint, client, args, isReliable);
        }
        finally
        {
            scratch.Dispose();
        }
    }
}

public sealed unsafe class Rpc<T1, T2> : RpcDefinition
{
    public Rpc(string name) : base(name)
    {
        Validate<T1>();
        Validate<T2>();
    }

    public void On(NetServer server, Action<NetClientId, T1, T2> handler) =>
        Register(server, (sender, args, count) =>
            handler(new NetClientId(sender), Decode<T1>(args, count, 0), Decode<T2>(args, count, 1)));

    public void On(NetClient client, Action<T1, T2> handler) =>
        Register(client, (_, args, count) => handler(Decode<T1>(args, count, 0), Decode<T2>(args, count, 1)));

    public void Send(NetClient client, T1 a, T2 b, bool isReliable = true) => Call(client, 0, a, b, isReliable);

    // to one client, or to all of them when `client` is null
    public void Send(NetServer server, NetClientId? client, T1 a, T2 b, bool isReliable = true) =>
        Call(server, Target(client), a, b, isReliable);

    void Call(NetEndpoint endpoint, ulong client, T1 a, T2 b, bool isReliable)
    {
        var scratch = new Scratch(Scratch.SizeFor(Size(a) + Size(b), 2));
        try
        {
            Span<Value> args = [Encode(a, ref scratch), Encode(b, ref scratch)];
            Send(endpoint, client, args, isReliable);
        }
        finally
        {
            scratch.Dispose();
        }
    }
}

public sealed unsafe class Rpc<T1, T2, T3> : RpcDefinition
{
    public Rpc(string name) : base(name)
    {
        Validate<T1>();
        Validate<T2>();
        Validate<T3>();
    }

    public void On(NetServer server, Action<NetClientId, T1, T2, T3> handler) =>
        Register(server, (sender, args, count) => handler(
            new NetClientId(sender),
            Decode<T1>(args, count, 0),
            Decode<T2>(args, count, 1),
            Decode<T3>(args, count, 2)));

    public void On(NetClient client, Action<T1, T2, T3> handler) =>
        Register(client, (_, args, count) =>
            handler(Decode<T1>(args, count, 0), Decode<T2>(args, count, 1), Decode<T3>(args, count, 2)));

    public void Send(NetClient client, T1 a, T2 b, T3 c, bool isReliable = true) => Call(client, 0, a, b, c, isReliable);

    // to one client, or to all of them when `client` is null
    public void Send(NetServer server, NetClientId? client, T1 a, T2 b, T3 c, bool isReliable = true) =>
        Call(server, Target(client), a, b, c, isReliable);

    void Call(NetEndpoint endpoint, ulong client, T1 a, T2 b, T3 c, bool isReliable)
    {
        var scratch = new Scratch(Scratch.SizeFor(Size(a) + Size(b) + Size(c), 3));
        try
        {
            Span<Value> args = [Encode(a, ref scratch), Encode(b, ref scratch), Encode(c, ref scratch)];
            Send(endpoint, client, args, isReliable);
        }
        finally
        {
            scratch.Dispose();
        }
    }
}
