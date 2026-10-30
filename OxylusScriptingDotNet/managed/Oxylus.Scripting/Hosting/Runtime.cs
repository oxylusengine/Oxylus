using System;
using System.Runtime.InteropServices;

using Oxylus.Interop;

namespace Oxylus.Hosting;

public static unsafe class Runtime
{
    internal static NativeApi Native;
    static IScriptRegistry? registry;
    static ComponentDescriptor[] components = [];
    static SystemDescriptor[] systems = [];

    // called by the generated ox_managed_init export under NativeAOT and by Bootstrap under hostfxr
    public static OxStatus Initialize(NativeApi* native, ManagedApi* managed, uint abiVersion, IScriptRegistry scripts)
    {
        if (abiVersion != Abi.Version || native->Size != (uint)sizeof(NativeApi))
        {
            return OxStatus.AbiMismatch;
        }

        Native = *native;
        registry = scripts;
        try
        {
            CoreComponents.AssignIndices();
            components = [.. CoreComponents.CreateDescriptors(), .. scripts.RegisterComponents(CoreComponents.Count)];
            systems = scripts.CreateSystems();
            Scenes.Register(scripts.CreateScriptTypes());
            scripts.OnLoad();
        }
        catch (Exception e)
        {
            Log.Error($"Scripts failed to load: {e}");
            ReleaseScripts();
            Detach();
            return OxStatus.Exception;
        }

        managed->Size = (uint)sizeof(ManagedApi);
        managed->Shutdown = &Shutdown;
        managed->AttachWorld = &AttachWorld;
        managed->DetachWorld = &DetachWorld;
        managed->AllocatedBytes = &AllocatedBytes;
        Scenes.Fill(managed);
        Callbacks.Fill(managed);
        return OxStatus.Ok;
    }

    [UnmanagedCallersOnly]
    static OxStatus Shutdown()
    {
        ReleaseScripts();
        Detach();
        return OxStatus.Ok;
    }

    [UnmanagedCallersOnly]
    static OxStatus AttachWorld(nint world)
    {
        try
        {
            return Attach(world);
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't attach the scripts to a world: {e}");
            return OxStatus.Exception;
        }
    }

    [UnmanagedCallersOnly]
    static OxStatus DetachWorld(nint world)
    {
        try
        {
            if (Worlds.Remove(world) is { } state)
            {
                DeleteSystems(state);
            }

            return OxStatus.Ok;
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't detach the scripts from a world: {e}");
            return OxStatus.Exception;
        }
    }

    [UnmanagedCallersOnly]
    static ulong AllocatedBytes() => (ulong)GC.GetAllocatedBytesForCurrentThread();

    static OxStatus Attach(nint world)
    {
        foreach (var component in components)
        {
            if (component.IsScriptDefined && Define(world, component) == 0)
            {
                Log.Error($"Couldn't define component {System.Text.Encoding.UTF8.GetString(component.Path)}");
                return OxStatus.InvalidArgument;
            }
        }

        var ids = new ulong[components.Length];
        var status = Bind(world, ids);
        if (status != OxStatus.Ok)
        {
            return status;
        }

        for (var i = 0; i < components.Length; i++)
        {
            if (components[i].IsScriptDefined && ids[i] != 0 && components[i].Size > 0 && !SetDefaults(world, ids[i], components[i]))
            {
                Log.Error($"Couldn't set the defaults of {System.Text.Encoding.UTF8.GetString(components[i].Path)}");
                return OxStatus.InvalidArgument;
            }
        }

        var state = new WorldState(world, ids);
        Worlds.Add(state);
        foreach (var system in systems)
        {
            var id = CreateSystem(world, system, ids);
            if (id == 0)
            {
                Log.Error($"Couldn't register system {System.Text.Encoding.UTF8.GetString(system.Name)}");
                Worlds.Remove(world);
                DeleteSystems(state);
                return OxStatus.InvalidArgument;
            }

            state.Systems.Add(id);
        }

        return OxStatus.Ok;
    }

    static ulong Define(nint world, ComponentDescriptor component)
    {
        var bytes = component.Path.Length;
        foreach (var member in component.Members)
        {
            bytes += member.Name.Length;
        }

        using var scratch = new Scratch(Scratch.SizeFor(bytes + component.Members.Length * sizeof(ComponentField), component.Members.Length + 2));
        var fields = scratch.Alloc<ComponentField>(component.Members.Length);
        for (var i = 0; i < component.Members.Length; i++)
        {
            fields[i] = new ComponentField { Name = scratch.String(component.Members[i].Name), Type = component.Members[i].Type };
        }

        return Native.DefineComponent(world, scratch.String(component.Path), fields, component.Members.Length);
    }

    static bool SetDefaults(nint world, ulong id, ComponentDescriptor component)
    {
        fixed (byte* defaults = component.Defaults)
        {
            return Native.SetComponentDefaults(world, id, defaults, component.Size) == OxStatus.Ok;
        }
    }

    static OxStatus Bind(nint world, ulong[] ids)
    {
        var bytes = components.Length * sizeof(ComponentLayout);
        var allocations = components.Length + 1;
        foreach (var component in components)
        {
            bytes += component.Path.Length + component.Members.Length * sizeof(MemberLayout);
            allocations += 1;
            foreach (var member in component.Members)
            {
                bytes += member.Name.Length;
                allocations += 1;
            }
        }

        using var scratch = new Scratch(Scratch.SizeFor(bytes, allocations));
        var layouts = scratch.Alloc<ComponentLayout>(components.Length);
        for (var i = 0; i < components.Length; i++)
        {
            var component = components[i];
            var members = scratch.Alloc<MemberLayout>(component.Members.Length);
            for (var m = 0; m < component.Members.Length; m++)
            {
                var member = component.Members[m];
                members[m] = new MemberLayout { Name = scratch.String(member.Name), Offset = member.Offset, Size = member.Size };
            }

            layouts[i] = new ComponentLayout
            {
                Path = scratch.String(component.Path),
                Members = members,
                MemberCount = (uint)component.Members.Length,
                Size = component.Size,
                Alignment = component.Alignment,
            };
        }

        fixed (ulong* data = ids)
        {
            return Native.BindComponents(world, layouts, components.Length, data);
        }
    }

    internal static ulong CreateSystem(nint world, SystemDescriptor system, ulong[] ids)
    {
        using var scratch = new Scratch(Scratch.SizeFor(system.Name.Length + system.Terms.Length * sizeof(Term), 2));
        var terms = scratch.Alloc<Term>(system.Terms.Length);
        for (var i = 0; i < system.Terms.Length; i++)
        {
            var index = system.Terms[i].ComponentIndex;
            terms[i] = new Term { Id = (uint)index < (uint)ids.Length ? ids[index] : 0, Access = system.Terms[i].Access };
        }

        return Native.CreateSystem(world, scratch.String(system.Name), system.Phase, terms, system.Terms.Length, system.Run);
    }

    static void DeleteSystems(WorldState state)
    {
        foreach (var system in state.Systems)
        {
            Native.EcsDelete(state.Handle, system);
        }

        state.Systems.Clear();
    }

    // drops every reference into the scripts assembly, systems included, so a collectible context holding it can be
    // collected
    internal static void ReleaseScripts()
    {
        Assets.ReleaseScripts();
        Callbacks.Release();
        Scenes.Release();
        foreach (var world in Worlds.All)
        {
            DeleteSystems(world);
        }

        Worlds.Clear();
        registry = null;
        components = [];
        systems = [];
    }

    // after this, calls into native code are no-ops
    internal static void Detach() => Native = default;
}

public static unsafe class Systems
{
    // the generated per-table loop calls this instead of letting an exception reach native code
    public static void Fail(TableView* view, string name, Exception exception)
    {
        Log.Error($"System {name} threw and is disabled: {exception}");
        Runtime.Native.EcsEnable(view->World, view->System, false);
    }
}
