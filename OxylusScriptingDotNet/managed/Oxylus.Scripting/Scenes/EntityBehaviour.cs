using System.Runtime.CompilerServices;

namespace Oxylus;

// the script on an entity whose ScriptComponent names it. It lives while the scene runs: OnCreate when the scene starts
// or the component is set, OnStart before its first update, OnDestroy when the component goes, the entity dies or the
// scene stops
public abstract class EntityBehaviour
{
    public Entity Entity { get; internal set; }
    public World World => Entity.World;

    internal ScriptType Type = null!;
    internal ListSlots Slots;
    internal bool IsStarted;
    internal bool IsFaulted;

    public virtual void OnCreate() { }
    public virtual void OnStart() { }
    public virtual void OnUpdate(float deltaTime) { }
    // once per physics step, before it
    public virtual void OnFixedUpdate(float deltaTime) { }
    public virtual void OnDestroy() { }
    // contacts this entity's body made in the last physics step, Self is this entity
    public virtual void OnContact(in Contact contact) { }
    public virtual void OnBodyActivated() { }
    public virtual void OnBodyDeactivated() { }
}

// where a behaviour sits in each of its world's dispatch lists, -1 when it isn't in one
[InlineArray(BehaviourList.Kinds)]
internal struct ListSlots
{
    int first;
}
