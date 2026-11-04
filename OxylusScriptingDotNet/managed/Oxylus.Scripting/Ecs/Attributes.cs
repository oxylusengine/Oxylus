using System;

namespace Oxylus;

// flecs' pipeline phases, in the order they run each frame
public enum Phase : int
{
    OnLoad = 0,
    PostLoad = 1,
    PreUpdate = 2,
    OnUpdate = 3,
    OnValidate = 4,
    PostUpdate = 5,
    PreStore = 6,
    OnStore = 7,
}

// a component defined in C#: the scene serializes it and the inspector edits it like an engine component.
// Fields may only be numbers, bool, Vector2/3/4 or Quaternion
[AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
public sealed class ComponentAttribute : Attribute;

// a static method run for every entity that has its `ref`/`in` component parameters. It may also take an
// `Entity` and a `float`, which gets the frame's delta time
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
public sealed class SystemAttribute(Phase phase = Phase.OnUpdate) : Attribute
{
    public Phase Phase { get; } = phase;
}
