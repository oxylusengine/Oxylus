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

public enum MotionType : uint
{
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
}

// the degrees of freedom a RigidBodyComponent leaves its body
[Flags]
public enum AllowedDofs : uint
{
    None = 0,
    TranslationX = 1 << 0,
    TranslationY = 1 << 1,
    TranslationZ = 1 << 2,
    RotationX = 1 << 3,
    RotationY = 1 << 4,
    RotationZ = 1 << 5,
    All = 0b111111,
    Plane2D = TranslationX | TranslationY | RotationZ,
}

public readonly struct Ray(Vector3 origin, Vector3 direction)
{
    public Vector3 Origin { get; } = origin;
    public Vector3 Direction { get; } = direction;

    public Vector3 GetPoint(float t) => Origin + Direction * t;
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
        var ray = new RayData
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

    public static bool Raycast(
        World world,
        in Ray ray,
        out RaycastHit hit,
        float maxDistance = float.PositiveInfinity,
        PhysicsLayers layers = PhysicsLayers.All,
        Entity ignore = default) =>
        Raycast(world, ray.Origin, ray.Direction, out hit, maxDistance, layers, ignore);

    extension(Entity entity)
    {
        // what a RigidBodyComponent becomes once the scene starts
        public Body Body => new(entity);

        public CharacterController Character => new(entity);

        // what a VehicleComponent becomes once the scene starts
        public Vehicle Vehicle => new(entity);

        // a wheel is a child of the vehicle's entity
        public VehicleWheel VehicleWheel => new(entity);
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
        throw new InvalidOperationException(
            $"{entity}'s {what} can't do that, a static body doesn't move, only a dynamic one takes forces, "
            + "only a body that can be kinematic or dynamic changes its motion type, and values must be ones jolt accepts");
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

    public MotionType MotionType => (MotionType)State.MotionType;
    public bool IsStatic => MotionType == MotionType.Static;
    public bool IsKinematic => MotionType == MotionType.Kinematic;
    public bool IsDynamic => MotionType == MotionType.Dynamic;

    // the one layer the body is in
    public PhysicsLayers ObjectLayer => (PhysicsLayers)(1u << (int)State.ObjectLayer);

    public float Friction => State.Friction;
    public float Restitution => State.Restitution;

    public bool IsSensor => HasFlag(BodyFlags.IsSensor);
    public bool CollideKinematicVsNonDynamic => HasFlag(BodyFlags.CollideKinematicVsNonDynamic);
    public bool UseManifoldReduction => HasFlag(BodyFlags.UseManifoldReduction);
    public bool ApplyGyroscopicForce => HasFlag(BodyFlags.ApplyGyroscopicForce);
    public bool EnhancedInternalEdgeRemoval => HasFlag(BodyFlags.EnhancedInternalEdgeRemoval);

    // Position is the shape's origin, this is where its mass is centred
    public Vector3 CenterOfMassPosition => State.CenterOfMass;

    // the box around the body's shapes
    public Aabb WorldBounds
    {
        get
        {
            var state = State;
            return new Aabb(state.BoundsMin, state.BoundsMax);
        }
    }

    // what forces and torques have gathered for the next step, zero for a body that isn't dynamic
    public Vector3 AccumulatedForce => State.AccumulatedForce;
    public Vector3 AccumulatedTorque => State.AccumulatedTorque;

    // the velocity of a world space point on the body, which spins around its centre of mass
    public Vector3 GetPointVelocity(Vector3 worldPoint)
    {
        var state = State;
        return state.LinearVelocity + Vector3.Cross(state.AngularVelocity, worldPoint - state.CenterOfMass);
    }

    // methods rather than setters, `entity.Body` is a value and C# won't call a setter on one
    public void SetLinearVelocity(Vector3 velocity) => Command(BodyCommand.SetLinearVelocity, velocity);
    public void SetAngularVelocity(Vector3 velocity) => Command(BodyCommand.SetAngularVelocity, velocity);
    public void AddLinearVelocity(Vector3 velocity) => Command(BodyCommand.AddLinearVelocity, velocity);

    // a force acts over the next step, an impulse changes the velocity at once
    public void AddForce(Vector3 force) => Command(BodyCommand.AddForce, force);
    public void AddImpulse(Vector3 impulse) => Command(BodyCommand.AddImpulse, impulse);
    public void AddTorque(Vector3 torque) => Command(BodyCommand.AddTorque, torque);
    public void AddAngularImpulse(Vector3 impulse) => Command(BodyCommand.AddAngularImpulse, impulse);

    // at a world space point, so one off the centre of mass spins the body too
    public void AddForce(Vector3 force, Vector3 worldPoint) =>
        Command(BodyCommand.AddForceAtPosition, new BodyArgs { Vector = force, Position = worldPoint });

    public void AddImpulse(Vector3 impulse, Vector3 worldPoint) =>
        Command(BodyCommand.AddImpulseAtPosition, new BodyArgs { Vector = impulse, Position = worldPoint });

    public void Activate() => Command(BodyCommand.Activate, Vector3.Zero);
    public void Deactivate() => Command(BodyCommand.Deactivate, Vector3.Zero);

    // sets the velocities that bring a kinematic or dynamic body to the target in deltaTime
    public void MoveKinematic(Vector3 targetPosition, Quaternion targetRotation, float deltaTime) =>
        Command(
            BodyCommand.MoveKinematic,
            new BodyArgs { Position = targetPosition, Rotation = targetRotation, DeltaTime = deltaTime });

    // these move the body at once and wake it
    public void SetPosition(Vector3 position) =>
        Command(BodyCommand.SetPosition, new BodyArgs { Position = position, Rotation = Quaternion.Identity });

    public void SetRotation(Quaternion rotation) =>
        Command(BodyCommand.SetRotation, new BodyArgs { Rotation = rotation });

    public void SetPositionAndRotation(Vector3 position, Quaternion rotation) =>
        Command(BodyCommand.SetPositionAndRotation, new BodyArgs { Position = position, Rotation = rotation });

    // only a body that was created kinematic or dynamic can change, and it wakes
    public void SetMotionType(MotionType type) => Set(BodyProperty.MotionType, (float)type);

    // exactly one layer, not a mask
    public void SetObjectLayer(PhysicsLayers layer)
    {
        if (!BitOperations.IsPow2((uint)layer))
        {
            throw new ArgumentException("A body is in exactly one object layer", nameof(layer));
        }

        Set(BodyProperty.ObjectLayer, BitOperations.TrailingZeroCount((uint)layer));
    }

    public void SetFriction(float friction) => Set(BodyProperty.Friction, friction);
    public void SetRestitution(float restitution) => Set(BodyProperty.Restitution, restitution);
    public void SetIsSensor(bool isSensor) => Set(BodyProperty.IsSensor, isSensor ? 1.0f : 0.0f);

    public void SetCollideKinematicVsNonDynamic(bool isEnabled) =>
        Set(BodyProperty.CollideKinematicVsNonDynamic, isEnabled ? 1.0f : 0.0f);

    public void SetUseManifoldReduction(bool isEnabled) =>
        Set(BodyProperty.UseManifoldReduction, isEnabled ? 1.0f : 0.0f);

    public void SetApplyGyroscopicForce(bool isEnabled) =>
        Set(BodyProperty.ApplyGyroscopicForce, isEnabled ? 1.0f : 0.0f);

    public void SetEnhancedInternalEdgeRemoval(bool isEnabled) =>
        Set(BodyProperty.EnhancedInternalEdgeRemoval, isEnabled ? 1.0f : 0.0f);

    BodyState State
    {
        get
        {
            BodyState state;
            Physics.Check(Runtime.Native.PhysicsBodyState(Entity.World.Handle, Entity.Id, &state), Entity, "physics body");
            return state;
        }
    }

    bool HasFlag(BodyFlags flag) => (State.Flags & flag) != 0;

    void Command(BodyCommand command, Vector3 value) =>
        Command(command, new BodyArgs { Vector = value, Rotation = Quaternion.Identity });

    void Command(BodyCommand command, BodyArgs args) =>
        Physics.Check(Runtime.Native.PhysicsBodyCommand(Entity.World.Handle, Entity.Id, command, &args), Entity, "physics body");

    void Set(BodyProperty property, float value) =>
        Physics.Check(Runtime.Native.PhysicsBodySet(Entity.World.Handle, Entity.Id, property, value), Entity, "physics body");
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

// an entity's vehicle, what a VehicleComponent becomes once the scene starts
public readonly unsafe struct Vehicle(Entity entity)
{
    public Entity Entity { get; } = entity;

    public bool Exists
    {
        get
        {
            VehicleState state;
            return Runtime.Native.PhysicsVehicleState(Entity.World.Handle, Entity.Id, &state) == OxStatus.Ok;
        }
    }

    public float EngineRpm => State.EngineRpm;

    // the crankshaft's speed, which never drops below the idle rpm while running, so it is no measure of how fast the vehicle goes
    public float EngineAngularVelocity => State.EngineAngularVelocity;

    public int Gear => State.Gear;

    // m/s along the chassis' forward axis, negative while rolling backwards
    public float ForwardSpeed => State.ForwardSpeed;

    VehicleState State
    {
        get
        {
            VehicleState state;
            Physics.Check(Runtime.Native.PhysicsVehicleState(Entity.World.Handle, Entity.Id, &state), Entity, "vehicle");
            return state;
        }
    }
}

// one wheel of a vehicle, an entity with a VehicleWheelComponent under the vehicle's entity
public readonly unsafe struct VehicleWheel(Entity entity)
{
    public Entity Entity { get; } = entity;

    // true while the wheel touches something
    public bool IsContacting => State.IsContacting != 0;

    // the slip ratio, and the slip angle in radians. Both are zero while the wheel is airborne
    public float LongitudinalSlip => State.LongitudinalSlip;
    public float LateralSlip => State.LateralSlip;

    WheelState State
    {
        get
        {
            WheelState state;
            Physics.Check(Runtime.Native.PhysicsWheelState(Entity.World.Handle, Entity.Id, &state), Entity, "vehicle wheel");
            return state;
        }
    }
}
