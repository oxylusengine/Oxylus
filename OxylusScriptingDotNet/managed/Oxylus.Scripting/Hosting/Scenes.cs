using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

using Oxylus.Interop;

namespace Oxylus.Hosting;

// the scene hooks native code calls, one call per world and hook. Every script call is guarded: a script that throws
// is logged and left out of later hooks, and no exception reaches native code
internal static unsafe class Scenes
{
    static ScriptType[] types = [];
    // the names native code reads after ListScriptTypes returns, so the GC must not move them
    static byte[][] pinnedNames = [];

    public static void Register(ScriptType[] scriptTypes)
    {
        types = scriptTypes;
        pinnedNames = new byte[scriptTypes.Length][];
        for (var i = 0; i < scriptTypes.Length; i++)
        {
            pinnedNames[i] = GC.AllocateArray<byte>(scriptTypes[i].Name.Length, pinned: true);
            scriptTypes[i].Name.CopyTo(pinnedNames[i], 0);
        }
    }

    public static void Fill(ManagedApi* managed)
    {
        managed->FindScriptType = &FindScriptType;
        managed->ListScriptTypes = &ListScriptTypes;
        managed->ScriptCreate = &ScriptCreate;
        managed->ScriptDestroy = &ScriptDestroy;
        managed->BehaviourAdd = &BehaviourAdd;
        managed->BehaviourRemove = &BehaviourRemove;
        managed->WorldStart = &WorldStart;
        managed->WorldStop = &WorldStop;
        managed->WorldUpdate = &WorldUpdate;
        managed->WorldFixedUpdate = &WorldFixedUpdate;
        managed->WorldRender = &WorldRender;
        managed->WorldContacts = &WorldContacts;
        managed->WorldBodyActivation = &WorldBodyActivation;
    }

    // drops every script without running its hooks, native code has already removed and stopped what it knew about
    public static void Release()
    {
        foreach (var world in Worlds.All)
        {
            world.Scripts.Clear();
            world.Behaviours.Clear();
            world.AllBehaviours.Clear();
            world.Updating.Clear();
            world.FixedUpdating.Clear();
            world.Starting.Clear();
            world.Deferred.Clear();
            world.Running.Clear();
            world.IsRunning = false;
        }

        types = [];
        pinnedNames = [];
    }

    static ScriptType? Find(StringView name)
    {
        var bytes = new ReadOnlySpan<byte>(name.Data, (int)name.Length);
        foreach (var type in types)
        {
            if (bytes.SequenceEqual(type.Name))
            {
                return type;
            }
        }

        return null;
    }

    static string Text(StringView name) => Encoding.UTF8.GetString(name.Data, (int)name.Length);

    static string NameOf(ScriptType type) => Encoding.UTF8.GetString(type.Name);

    [UnmanagedCallersOnly]
    static ScriptKind FindScriptType(StringView name)
    {
        try
        {
            return Find(name)?.Kind ?? ScriptKind.None;
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't look up script type {Text(name)}: {e}");
            return ScriptKind.None;
        }
    }

    [UnmanagedCallersOnly]
    static int ListScriptTypes(ScriptTypeInfo* infos, int capacity)
    {
        for (var i = 0; i < types.Length && i < capacity; i++)
        {
            var name = pinnedNames[i];
            infos[i] = new ScriptTypeInfo
            {
                Name = new StringView((byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(name)), name.Length),
                Kind = types[i].Kind,
            };
        }

        return types.Length;
    }

    [UnmanagedCallersOnly]
    static OxStatus ScriptCreate(nint world, StringView name, ulong* handle)
    {
        *handle = 0;
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                Log.Error($"Scene script {Text(name)} was created in a world the scripts aren't attached to");
                return OxStatus.InvalidArgument;
            }

            if (Find(name) is not { Kind: ScriptKind.SceneScript } type)
            {
                Log.Error($"There is no C# scene script {Text(name)}");
                return OxStatus.NotFound;
            }

            var script = (SceneScript)type.Create();
            script.World = new World(world);
            script.Type = type;
            foreach (var system in type.Systems)
            {
                var id = Runtime.CreateSystem(world, system, state.ComponentIds);
                if (id == 0)
                {
                    Log.Error($"Couldn't register system {Encoding.UTF8.GetString(system.Name)} of {NameOf(type)}");
                    DeleteSystems(script);
                    return OxStatus.InvalidArgument;
                }

                script.Systems.Add(id);
            }

            state.Scripts.Add(script);
            try
            {
                script.OnAdd();
            }
            catch (Exception e)
            {
                Fault(script, nameof(SceneScript.OnAdd), e);
            }

            if (state.IsRunning)
            {
                Start(script);
            }

            *handle = (ulong)GCHandle.ToIntPtr(GCHandle.Alloc(script));
            return OxStatus.Ok;
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't create scene script {Text(name)}: {e}");
            return OxStatus.Exception;
        }
    }

    [UnmanagedCallersOnly]
    static void ScriptDestroy(ulong handle)
    {
        try
        {
            var gcHandle = GCHandle.FromIntPtr((nint)handle);
            var script = (SceneScript)gcHandle.Target!;
            gcHandle.Free();

            if (!script.IsFaulted)
            {
                try
                {
                    if (script.IsStarted)
                    {
                        script.OnStop();
                    }

                    script.OnRemove();
                }
                catch (Exception e)
                {
                    Fault(script, nameof(SceneScript.OnRemove), e);
                }
            }

            DeleteSystems(script);
            Worlds.TryGet(script.World.Handle)?.Scripts.Remove(script);
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't destroy a scene script: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static OxStatus BehaviourAdd(nint world, ulong entity, StringView name)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return OxStatus.InvalidArgument;
            }

            var type = Find(name);
            if (type is not { Kind: ScriptKind.Behaviour })
            {
                Log.Error($"There is no C# behaviour {Text(name)}");
                RemoveBehaviour(state, entity);
                return OxStatus.NotFound;
            }

            if (state.Behaviours.TryGetValue(entity, out var existing))
            {
                if (existing.Type == type)
                {
                    return OxStatus.Ok;
                }

                RemoveBehaviour(state, entity);
            }

            var behaviour = (Behaviour)type.Create();
            behaviour.Type = type;
            behaviour.Entity = new Entity(world, entity);
            for (var kind = 0; kind < BehaviourList.Kinds; kind++)
            {
                behaviour.Slots[kind] = -1;
            }

            state.Behaviours.Add(entity, behaviour);
            state.AllBehaviours.Add(behaviour);
            if ((type.Hooks & ScriptHooks.Update) != 0)
            {
                state.Updating.Add(behaviour);
            }

            if ((type.Hooks & ScriptHooks.FixedUpdate) != 0)
            {
                state.FixedUpdating.Add(behaviour);
            }

            try
            {
                behaviour.OnCreate();
            }
            catch (Exception e)
            {
                Fault(state, behaviour, nameof(Behaviour.OnCreate), e);
            }

            state.Starting.Add(behaviour);
            return OxStatus.Ok;
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't create behaviour {Text(name)}: {e}");
            return OxStatus.Exception;
        }
    }

    [UnmanagedCallersOnly]
    static void BehaviourRemove(nint world, ulong entity)
    {
        try
        {
            if (Worlds.TryGet(world) is { } state)
            {
                RemoveBehaviour(state, entity);
            }
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't remove a behaviour: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldStart(nint world)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            state.IsRunning = true;
            for (var i = 0; i < state.Scripts.Count; i++)
            {
                Start(state.Scripts[i]);
            }

            StartPending(state);
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't start the scripts: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldStop(nint world)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            for (var i = 0; i < state.Scripts.Count; i++)
            {
                var script = state.Scripts[i];
                if (!script.IsStarted)
                {
                    continue;
                }

                script.IsStarted = false;
                if (script.IsFaulted)
                {
                    continue;
                }

                try
                {
                    script.OnStop();
                }
                catch (Exception e)
                {
                    Fault(script, nameof(SceneScript.OnStop), e);
                }
            }

            var behaviours = state.AllBehaviours;
            var count = behaviours.BeginDispatch();
            try
            {
                for (var i = 0; i < count; i++)
                {
                    if (behaviours[i] is { } behaviour)
                    {
                        RemoveBehaviour(state, behaviour.Entity.Id);
                    }
                }
            }
            finally
            {
                behaviours.EndDispatch();
            }

            // whatever an OnDestroy created doesn't outlive the stop either
            state.Behaviours.Clear();
            state.AllBehaviours.Clear();
            state.Updating.Clear();
            state.FixedUpdating.Clear();
            state.Starting.Clear();
            state.IsRunning = false;
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't stop the scripts: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldUpdate(nint world, float deltaTime)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            RunDeferred(state);
            StartPending(state);
            for (var i = 0; i < state.Scripts.Count; i++)
            {
                var script = state.Scripts[i];
                if (script.IsFaulted)
                {
                    continue;
                }

                try
                {
                    script.OnUpdate(deltaTime);
                }
                catch (Exception e)
                {
                    Fault(script, nameof(SceneScript.OnUpdate), e);
                }
            }

            var behaviours = state.Updating;
            var count = behaviours.BeginDispatch();
            try
            {
                for (var i = 0; i < count; i++)
                {
                    if (behaviours[i] is not { } behaviour)
                    {
                        continue;
                    }

                    try
                    {
                        behaviour.OnUpdate(deltaTime);
                    }
                    catch (Exception e)
                    {
                        Fault(state, behaviour, nameof(Behaviour.OnUpdate), e);
                    }
                }
            }
            finally
            {
                behaviours.EndDispatch();
            }
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't update the scripts: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldFixedUpdate(nint world, float deltaTime)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            StartPending(state);
            for (var i = 0; i < state.Scripts.Count; i++)
            {
                var script = state.Scripts[i];
                if (script.IsFaulted)
                {
                    continue;
                }

                try
                {
                    script.OnFixedUpdate(deltaTime);
                }
                catch (Exception e)
                {
                    Fault(script, nameof(SceneScript.OnFixedUpdate), e);
                }
            }

            var behaviours = state.FixedUpdating;
            var count = behaviours.BeginDispatch();
            try
            {
                for (var i = 0; i < count; i++)
                {
                    if (behaviours[i] is not { } behaviour)
                    {
                        continue;
                    }

                    try
                    {
                        behaviour.OnFixedUpdate(deltaTime);
                    }
                    catch (Exception e)
                    {
                        Fault(state, behaviour, nameof(Behaviour.OnFixedUpdate), e);
                    }
                }
            }
            finally
            {
                behaviours.EndDispatch();
            }
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't run the scripts' fixed update: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldRender(nint world, uint width, uint height)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            for (var i = 0; i < state.Scripts.Count; i++)
            {
                var script = state.Scripts[i];
                if (script.IsFaulted)
                {
                    continue;
                }

                try
                {
                    script.OnRender(width, height);
                }
                catch (Exception e)
                {
                    Fault(script, nameof(SceneScript.OnRender), e);
                }
            }
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't render the scripts: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldContacts(nint world, PhysicsContact* contacts, int count)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            for (var c = 0; c < count; c++)
            {
                ref readonly var raw = ref contacts[c];
                var contact = new Contact(
                    raw.Event,
                    EntityOf(world, raw.Entity1),
                    EntityOf(world, raw.Entity2),
                    raw.Point,
                    raw.Normal,
                    raw.PenetrationDepth);

                for (var i = 0; i < state.Scripts.Count; i++)
                {
                    var script = state.Scripts[i];
                    if (script.IsFaulted)
                    {
                        continue;
                    }

                    try
                    {
                        script.OnContact(in contact);
                    }
                    catch (Exception e)
                    {
                        Fault(script, nameof(SceneScript.OnContact), e);
                    }
                }

                if (raw.Entity1 != 0 && state.Behaviours.TryGetValue(raw.Entity1, out var first))
                {
                    Contact(state, first, in contact);
                }

                if (raw.Entity2 != 0 && state.Behaviours.TryGetValue(raw.Entity2, out var second))
                {
                    var flipped = contact.Flipped();
                    Contact(state, second, in flipped);
                }
            }
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't hand the scripts their contacts: {e}");
        }
    }

    [UnmanagedCallersOnly]
    static void WorldBodyActivation(nint world, ulong entity, bool isActivated)
    {
        try
        {
            if (Worlds.TryGet(world) is not { } state)
            {
                return;
            }

            var target = EntityOf(world, entity);
            for (var i = 0; i < state.Scripts.Count; i++)
            {
                var script = state.Scripts[i];
                if (script.IsFaulted)
                {
                    continue;
                }

                try
                {
                    if (isActivated)
                    {
                        script.OnBodyActivated(target);
                    }
                    else
                    {
                        script.OnBodyDeactivated(target);
                    }
                }
                catch (Exception e)
                {
                    Fault(script, isActivated ? nameof(SceneScript.OnBodyActivated) : nameof(SceneScript.OnBodyDeactivated), e);
                }
            }

            if (entity == 0 || !state.Behaviours.TryGetValue(entity, out var behaviour) || behaviour.IsFaulted)
            {
                return;
            }

            try
            {
                if (isActivated)
                {
                    behaviour.OnBodyActivated();
                }
                else
                {
                    behaviour.OnBodyDeactivated();
                }
            }
            catch (Exception e)
            {
                Fault(state, behaviour, isActivated ? nameof(Behaviour.OnBodyActivated) : nameof(Behaviour.OnBodyDeactivated), e);
            }
        }
        catch (Exception e)
        {
            Log.Error($"Couldn't hand the scripts a body activation: {e}");
        }
    }

    static Entity EntityOf(nint world, ulong entity) => entity == 0 ? default : new Entity(world, entity);

    // swapped out first, an action that defers another queues it for the next update
    static void RunDeferred(WorldState state)
    {
        if (state.Deferred.Count == 0)
        {
            return;
        }

        (state.Deferred, state.Running) = (state.Running, state.Deferred);
        foreach (var action in state.Running)
        {
            try
            {
                action();
            }
            catch (Exception e)
            {
                Log.Error($"A deferred action threw: {e}");
            }
        }

        state.Running.Clear();
    }

    static void Contact(WorldState state, Behaviour behaviour, in Contact contact)
    {
        if (behaviour.IsFaulted)
        {
            return;
        }

        try
        {
            behaviour.OnContact(in contact);
        }
        catch (Exception e)
        {
            Fault(state, behaviour, nameof(Behaviour.OnContact), e);
        }
    }

    static void Start(SceneScript script)
    {
        if (script.IsStarted || script.IsFaulted)
        {
            return;
        }

        script.IsStarted = true;
        try
        {
            script.OnStart();
        }
        catch (Exception e)
        {
            Fault(script, nameof(SceneScript.OnStart), e);
        }
    }

    // OnStart can create behaviours, which are appended and started in the same pass
    static void StartPending(WorldState state)
    {
        var starting = state.Starting;
        for (var i = 0; i < starting.Count; i++)
        {
            var behaviour = starting[i];
            if (behaviour.IsStarted || behaviour.IsFaulted || behaviour.Slots[BehaviourList.All] < 0)
            {
                continue;
            }

            behaviour.IsStarted = true;
            try
            {
                behaviour.OnStart();
            }
            catch (Exception e)
            {
                Fault(state, behaviour, nameof(Behaviour.OnStart), e);
            }
        }

        starting.Clear();
    }

    static void RemoveBehaviour(WorldState state, ulong entity)
    {
        if (!state.Behaviours.Remove(entity, out var behaviour))
        {
            return;
        }

        state.AllBehaviours.Remove(behaviour);
        state.Updating.Remove(behaviour);
        state.FixedUpdating.Remove(behaviour);
        try
        {
            behaviour.OnDestroy();
        }
        catch (Exception e)
        {
            Log.Error($"{NameOf(behaviour.Type)}.OnDestroy on {behaviour.Entity} threw: {e}");
        }
    }

    static void DeleteSystems(SceneScript script)
    {
        foreach (var system in script.Systems)
        {
            Runtime.Native.EcsDelete(script.World.Handle, system);
        }

        script.Systems.Clear();
    }

    static void Fault(SceneScript script, string hook, Exception exception)
    {
        script.IsFaulted = true;
        Log.Error($"{NameOf(script.Type)}.{hook} threw and the script is disabled: {exception}");
    }

    static void Fault(WorldState state, Behaviour behaviour, string hook, Exception exception)
    {
        behaviour.IsFaulted = true;
        state.Updating.Remove(behaviour);
        state.FixedUpdating.Remove(behaviour);
        Log.Error($"{NameOf(behaviour.Type)}.{hook} on {behaviour.Entity} threw and the behaviour is disabled: {exception}");
    }
}
