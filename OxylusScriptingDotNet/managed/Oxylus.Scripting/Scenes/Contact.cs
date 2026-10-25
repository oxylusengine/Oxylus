using System.Numerics;

namespace Oxylus;

public enum ContactEvent : uint
{
    Added = 0,
    Persisted = 1,
    Removed = 2,
}

// a contact seen from Self. Point and Normal are world space, Normal points from Self into Other, and a removed
// contact carries neither. Either entity is default when its body belongs to no entity
public readonly struct Contact(ContactEvent contactEvent, Entity self, Entity other, Vector3 point, Vector3 normal, float penetrationDepth)
{
    public ContactEvent Event { get; } = contactEvent;
    public Entity Self { get; } = self;
    public Entity Other { get; } = other;
    public Vector3 Point { get; } = point;
    public Vector3 Normal { get; } = normal;
    public float PenetrationDepth { get; } = penetrationDepth;

    internal Contact Flipped() => new(Event, Other, Self, Point, -Normal, PenetrationDepth);
}
