using System;
using System.Diagnostics.CodeAnalysis;
using System.Numerics;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// the object layers of Physics/PhysicsInterfaces.hpp, as a mask
[Flags]
public enum PhysicsLayers : uint
{
    NonMoving = 1 << 0,
    Moving = 1 << 1,
    All = ~0u,
}

public readonly struct RaycastHit(Entity entity, Vector3 point, Vector3 normal, float distance)
{
    public Entity Entity { get; } = entity;
    public Vector3 Point { get; } = point;
    // the surface's, at Point
    public Vector3 Normal { get; } = normal;
    public float Distance { get; } = distance;
}

public static unsafe class Physics
{
    // the closest hit against the shapes themselves, not their bounding boxes. Only a running scene has bodies
    public static bool Raycast(
        World world,
        Vector3 origin,
        Vector3 direction,
        out RaycastHit hit,
        float maxDistance = float.PositiveInfinity,
        PhysicsLayers layers = PhysicsLayers.All,
        Entity ignore = default)
    {
        var ray = new Ray
        {
            Origin = origin,
            MaxDistance = maxDistance,
            Direction = direction,
            LayerMask = (uint)layers,
            IgnoreEntity = ignore.Id,
        };
        RayHit result;
        if (!Runtime.Native.PhysicsRaycast(world.Handle, &ray, &result))
        {
            hit = default;
            return false;
        }

        hit = new RaycastHit(
            result.Entity == 0 ? default : new Entity(world.Handle, result.Entity),
            result.Point,
            result.Normal,
            result.Distance);
        return true;
    }

    extension(Entity entity)
    {
        // what a RigidBodyComponent becomes once the scene starts
        public Body Body => new(entity);

        public CharacterController Character => new(entity);
    }

    internal static void Check(OxStatus status, Entity entity, string what)
    {
        switch (status)
        {
            case OxStatus.Ok:
                return;
            case OxStatus.NotFound:
                ThrowMissing(entity, what);
                break;
            default:
                ThrowRefused(entity, what);
                break;
        }
    }

    [DoesNotReturn]
    static void ThrowMissing(Entity entity, string what) =>
        throw new InvalidOperationException($"{entity} has no {what}, one exists for its component while the scene runs");

    [DoesNotReturn]
    static void ThrowRefused(Entity entity, string what) =>
        throw new InvalidOperationException($"{entity}'s {what} can't do that, a static body doesn't move and only a dynamic one takes forces");
}

// an entity's rigid body, through jolt's locking body interface. Positions are world space
public readonly unsafe struct Body(Entity entity)
{
    public Entity Entity { get; } = entity;

    public bool Exists
    {
        get
        {
            BodyState state;
            return Runtime.Native.PhysicsBodyState(Entity.World.Handle, Entity.Id, &state) == OxStatus.Ok;
        }
    }

    public Vector3 Position => State.Position;
    public Quaternion Rotation => State.Rotation;
    public bool IsActive => State.IsActive != 0;

    public Vector3 LinearVelocity => State.LinearVelocity;
    public Vector3 AngularVelocity => State.AngularVelocity;

    // methods rather than setters, `entity.Body` is a value and C# won't call a setter on one
    public void SetLinearVelocity(Vector3 velocity) => Command(BodyCommand.SetLinearVelocity, velocity);
    public void SetAngularVelocity(Vector3 velocity) => Command(BodyCommand.SetAngularVelocity, velocity);

    // a force acts over the next step, an impulse changes the velocity at once
    public void AddForce(Vector3 force) => Command(BodyCommand.AddForce, force);
    public void AddImpulse(Vector3 impulse) => Command(BodyCommand.AddImpulse, impulse);
    public void AddTorque(Vector3 torque) => Command(BodyCommand.AddTorque, torque);
    public void AddAngularImpulse(Vector3 impulse) => Command(BodyCommand.AddAngularImpulse, impulse);
    public void Activate() => Command(BodyCommand.Activate, Vector3.Zero);
    public void Deactivate() => Command(BodyCommand.Deactivate, Vector3.Zero);

    BodyState State
    {
        get
        {
            BodyState state;
            Physics.Check(Runtime.Native.PhysicsBodyState(Entity.World.Handle, Entity.Id, &state), Entity, "physics body");
            return state;
        }
    }

    void Command(BodyCommand command, Vector3 value) =>
        Physics.Check(Runtime.Native.PhysicsBodyCommand(Entity.World.Handle, Entity.Id, command, &value), Entity, "physics body");
}

public enum GroundState : uint
{
    OnGround = 0,
    OnSteepGround = 1,
    NotSupported = 2,
    InAir = 3,
}

// an entity's character controller, what a CharacterControllerComponent becomes once the scene starts
public readonly unsafe struct CharacterController(Entity entity)
{
    public Entity Entity { get; } = entity;

    public Vector3 Position => State.Position;
    public Vector3 LinearVelocity => State.LinearVelocity;

    public GroundState GroundState => (GroundState)State.GroundState;
    public bool IsGrounded => GroundState == GroundState.OnGround;
    public Vector3 GroundNormal => State.GroundNormal;

    public void SetPosition(Vector3 position) => Command(CharacterCommand.SetPosition, position);
    public void SetLinearVelocity(Vector3 velocity) => Command(CharacterCommand.SetLinearVelocity, velocity);
    public void AddLinearVelocity(Vector3 velocity) => Command(CharacterCommand.AddLinearVelocity, velocity);
    public void AddImpulse(Vector3 impulse) => Command(CharacterCommand.AddImpulse, impulse);

    CharacterState State
    {
        get
        {
            CharacterState state;
            Physics.Check(Runtime.Native.PhysicsCharacterState(Entity.World.Handle, Entity.Id, &state), Entity, "character controller");
            return state;
        }
    }

    void Command(CharacterCommand command, Vector3 value) =>
        Physics.Check(Runtime.Native.PhysicsCharacterCommand(Entity.World.Handle, Entity.Id, command, &value), Entity, "character controller");
}
