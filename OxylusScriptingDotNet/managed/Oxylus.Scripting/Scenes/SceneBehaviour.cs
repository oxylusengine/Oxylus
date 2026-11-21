using System.Collections.Generic;
using System.Diagnostics;

namespace Oxylus;

// one per scene that lists it in its "scripts", found by its full name, "Game.LevelLogic". [System] methods declared in
// it run only in the scenes that have it, from OnAdd until OnRemove
[DebuggerDisplay("{DebuggerDisplay,nq}")]
public abstract class SceneBehaviour
{
    public World World { get; internal set; }

    internal ScriptType Type = null!;
    internal readonly List<ulong> Systems = [];
    internal bool IsStarted;
    internal bool IsFaulted;

    [DebuggerBrowsable(DebuggerBrowsableState.Never)]
    internal string DebuggerDisplay => IsFaulted ? $"{GetType().Name} (faulted)" : GetType().Name;

    public virtual void OnAdd() { }
    public virtual void OnRemove() { }
    public virtual void OnStart() { }
    public virtual void OnStop() { }
    public virtual void OnUpdate(float deltaTime) { }
    // once per physics step, before it
    public virtual void OnFixedUpdate(float deltaTime) { }
    public virtual void OnRender(uint width, uint height) { }
    // every contact in the scene after each physics step, Self and Other are the two entities as jolt paired them
    public virtual void OnContact(in Contact contact) { }
    public virtual void OnBodyActivated(Entity entity) { }
    public virtual void OnBodyDeactivated(Entity entity) { }
}
