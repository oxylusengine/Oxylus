using System;
using System.Runtime.InteropServices;

using Oxylus.Interop;

namespace Oxylus.Hosting;

// what RmlUi and the network call back into, on the main thread. A handler that throws is logged and the call returns
internal static unsafe class Callbacks
{
    public static void Fill(ManagedApi* managed)
    {
        managed->UiEvent = &UiEvent;
        managed->UiModelGet = &UiModelGet;
        managed->UiModelSet = &UiModelSet;
        managed->UiModelEvent = &UiModelEvent;
        managed->NetRpc = &NetRpc;
        managed->NetEvent = &NetEventCallback;
        managed->NetSnapshot = &NetSnapshotCallback;
        managed->WindowResized = &WindowResized;
    }

    // the native side has already dropped the listeners, models and endpoints these name
    public static void Release()
    {
        Ui.Release();
        NetEndpoint.Release();
        Window.Release();
    }

    [UnmanagedCallersOnly]
    static void WindowResized(uint width, uint height)
    {
        try
        {
            Window.OnResized(width, height);
        }
        catch (Exception e)
        {
            Log.Error($"A window resize handler threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void UiEvent(ulong listener, UiEventData* data)
    {
        try
        {
            if (Ui.Listeners.TryGetValue(listener, out var handler))
            {
                handler(new Oxylus.UiEvent(data));
            }
        }
        catch (Exception e)
        {
            Log.Error($"A UI event listener threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void UiModelGet(ulong model, int binding, Value* value)
    {
        *value = default;
        try
        {
            if (Ui.Models.TryGetValue(model, out var found))
            {
                found.Get(binding, value);
            }
        }
        catch (Exception e)
        {
            *value = default;
            Log.Error($"A data model getter threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void UiModelSet(ulong model, int binding, Value* value)
    {
        try
        {
            if (Ui.Models.TryGetValue(model, out var found))
            {
                found.Set(binding, value);
            }
        }
        catch (Exception e)
        {
            Log.Error($"A data model setter threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void UiModelEvent(ulong model, int binding, UiEventData* data, Value* args, int count)
    {
        try
        {
            if (Ui.Models.TryGetValue(model, out var found))
            {
                found.Event(binding, data, args, count);
            }
        }
        catch (Exception e)
        {
            Log.Error($"A data model event threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void NetRpc(ulong endpoint, int binding, ulong sender, Value* args, int count)
    {
        try
        {
            if (NetEndpoint.Open.TryGetValue(endpoint, out var found) && (uint)binding < (uint)found.Rpcs.Count)
            {
                found.Rpcs[binding](sender, args, count);
            }
        }
        catch (Exception e)
        {
            Log.Error($"An RPC handler threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void NetEventCallback(ulong endpoint, NetEvent kind, ulong peer, uint reason)
    {
        try
        {
            if (NetEndpoint.Open.TryGetValue(endpoint, out var found))
            {
                found.OnEvent(kind, peer, reason);
            }
        }
        catch (Exception e)
        {
            Log.Error($"A network event handler threw: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void NetSnapshotCallback(ulong endpoint, byte sequence, ulong* changed, int changedCount, ulong* removed, int removedCount)
    {
        try
        {
            if (NetEndpoint.Open.TryGetValue(endpoint, out var found))
            {
                found.OnSnapshot(sequence, new ReadOnlySpan<ulong>(changed, changedCount), new ReadOnlySpan<ulong>(removed, removedCount));
            }
        }
        catch (Exception e)
        {
            Log.Error($"A scene snapshot handler threw: {e}");
        }
    }
}
