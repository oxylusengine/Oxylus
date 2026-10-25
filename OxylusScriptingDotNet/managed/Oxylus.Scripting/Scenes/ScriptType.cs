using System;

using Oxylus.Interop;

namespace Oxylus;

// the hooks a behaviour type overrides, so the dispatch lists only hold the behaviours that do something there
[Flags]
public enum ScriptHooks
{
    None = 0,
    Update = 1 << 0,
    FixedUpdate = 1 << 1,
}

// a SceneScript or Behaviour class, described by the generator so nothing is found by reflection
public sealed class ScriptType(byte[] name, ScriptKind kind, ScriptHooks hooks, Func<object> create, SystemDescriptor[] systems)
{
    public byte[] Name { get; } = name;
    public ScriptKind Kind { get; } = kind;
    public ScriptHooks Hooks { get; } = hooks;
    public Func<object> Create { get; } = create;
    public SystemDescriptor[] Systems { get; } = systems;
}

public static class BehaviourType<T> where T : Behaviour
{
    public static byte[]? Name;
}
