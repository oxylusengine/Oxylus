using System;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

using Oxylus.Interop;

namespace Oxylus.Hosting;

// the hostfxr entry point, the editor's way in. A NativeAOT build never reaches it, the generated export calls
// Runtime.Initialize directly
public static unsafe class Bootstrap
{
    const int MaxCollectAttempts = 10;

    static ScriptLoadContext? context;

    [UnmanagedCallersOnly]
    public static OxStatus Load(byte* path, int length, NativeApi* native, ManagedApi* managed, uint abiVersion)
    {
        try
        {
            var status = LoadScripts(Encoding.UTF8.GetString(path, length), native, managed, abiVersion);
            if (status == OxStatus.Ok)
            {
                managed->Shutdown = &Unload;
            }

            return status;
        }
        catch (Exception e)
        {
            LogError(native, $"Couldn't load scripts: {e}");
            return OxStatus.Exception;
        }
    }

    // Native isn't attached on this path, so errors go straight through the table we were handed
    static void LogError(NativeApi* native, string text)
    {
        if (native->Log is null)
        {
            return;
        }

        var message = Encoding.UTF8.GetBytes(text);
        fixed (byte* bytes = message)
        {
            native->Log(LogLevel.Error, bytes, (nuint)message.Length);
        }
    }

    [UnconditionalSuppressMessage("Trimming", "IL2026", Justification = "hostfxr only, NativeAOT builds never call it")]
    [UnconditionalSuppressMessage("Trimming", "IL2072", Justification = "hostfxr only, NativeAOT builds never call it")]
    [MethodImpl(MethodImplOptions.NoInlining)]
    static OxStatus LoadScripts(string path, NativeApi* native, ManagedApi* managed, uint abiVersion)
    {
        var loadContext = new ScriptLoadContext(path);

        // from a stream, so the file isn't locked and the editor can rebuild it while it is loaded
        Assembly assembly;
        using (var image = File.OpenRead(path))
        {
            var symbolsPath = Path.ChangeExtension(path, ".pdb");
            using var symbols = File.Exists(symbolsPath) ? File.OpenRead(symbolsPath) : null;
            assembly = loadContext.LoadFromStream(image, symbols);
        }

        var registryType = assembly.GetType("Oxylus.Generated.ScriptRegistry");
        if (registryType is null)
        {
            LogError(native, $"{path} has no generated Oxylus.Generated.ScriptRegistry, does it reference the Oxylus generator?");
            loadContext.Unload();
            return OxStatus.NotFound;
        }

        var registry = (IScriptRegistry)Activator.CreateInstance(registryType)!;
        var status = Runtime.Initialize(native, managed, abiVersion, registry);
        if (status != OxStatus.Ok)
        {
            loadContext.Unload();
            return status;
        }

        context = loadContext;
        return OxStatus.Ok;
    }

    [UnmanagedCallersOnly]
    static OxStatus Unload()
    {
        Runtime.ReleaseScripts();

        var unloaded = UnloadContext();
        for (var attempt = 0; unloaded.IsAlive && attempt < MaxCollectAttempts; attempt++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }

        if (unloaded.IsAlive)
        {
            Log.Warn("The scripts assembly is still referenced after unloading, it stays in memory until that goes away");
        }

        Runtime.Detach();
        return OxStatus.Ok;
    }

    // kept out of Unload so no local in that frame keeps the context alive
    [MethodImpl(MethodImplOptions.NoInlining)]
    static WeakReference UnloadContext()
    {
        var unloaded = new WeakReference(context);
        context?.Unload();
        context = null;
        return unloaded;
    }
}

sealed class ScriptLoadContext(string mainAssemblyPath) : AssemblyLoadContext(isCollectible: true)
{
    readonly AssemblyDependencyResolver resolver = new(mainAssemblyPath);

    [UnconditionalSuppressMessage("Trimming", "IL2026", Justification = "hostfxr only, NativeAOT builds never call it")]
    protected override Assembly? Load(AssemblyName name)
    {
        // falls through to the default context, which owns the one copy the engine talks to
        if (name.Name == "Oxylus.Scripting")
        {
            return null;
        }

        var path = resolver.ResolveAssemblyToPath(name);
        return path is null ? null : LoadFromAssemblyPath(path);
    }
}
