using System.Numerics;
using System.Runtime.InteropServices;

namespace Oxylus.Interop;

// mirrors OxylusScriptingDotNet/include/Scripting/DotNetAbi.hpp
public static class Abi
{
    public const uint Version = 5;
}

public enum OxStatus : int
{
    Ok = 0,
    AbiMismatch = 1,
    Exception = 2,
    NotFound = 3,
    LayoutMismatch = 4,
    InvalidArgument = 5,
}

public enum LogLevel : int
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

public enum TermAccess : int
{
    InOut = 0,
    In = 1,
    Out = 2,
}

public enum ScriptKind : int
{
    None = 0,
    SceneScript = 1,
    Behaviour = 2,
}

// matches ComponentFieldType in Scene/Scene.hpp
public enum ComponentFieldType : int
{
    F32,
    F64,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    Bool,
    String,
    Vec2,
    Vec3,
    Vec4,
    Quat,
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct StringView
{
    public byte* Data;
    public nuint Length;

    public StringView(byte* data, int length)
    {
        Data = data;
        Length = (nuint)length;
    }
}

[StructLayout(LayoutKind.Sequential)]
public struct Term
{
    public ulong Id;
    public TermAccess Access;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct TableView
{
    public nint World;
    public ulong System;
    public ulong* Entities;
    public void** Fields;
    public int Count;
    public float DeltaTime;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct MemberLayout
{
    public StringView Name;
    public uint Offset;
    public uint Size;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ComponentLayout
{
    public StringView Path;
    public MemberLayout* Members;
    public uint MemberCount;
    public uint Size;
    public uint Alignment;
}

[StructLayout(LayoutKind.Sequential)]
public struct ComponentField
{
    public StringView Name;
    public ComponentFieldType Type;
}

public enum InputState : int
{
    Pressed = 0,
    Released = 1,
    Held = 2,
}

public enum InputContextOp : int
{
    Set = 0,
    Push = 1,
    Pop = 2,
}

[StructLayout(LayoutKind.Sequential)]
public struct InputCodeData
{
    public byte Type;
    public byte AxisDirection;
    public ushort Mod;
    public int Code;
}

[StructLayout(LayoutKind.Sequential)]
public struct MouseState
{
    public Vector2 Position;
    public Vector2 Delta;
    public float Scroll;
    public uint IsMoved;
}

[StructLayout(LayoutKind.Sequential)]
public struct Ray
{
    public Vector3 Origin;
    public float MaxDistance;
    public Vector3 Direction;
    public uint LayerMask;
    public ulong IgnoreEntity;
}

[StructLayout(LayoutKind.Sequential)]
public struct RayHit
{
    public ulong Entity;
    public Vector3 Point;
    public float Distance;
    public Vector3 Normal;
    public uint Padding;
}

public enum BodyCommand : int
{
    SetLinearVelocity = 0,
    SetAngularVelocity = 1,
    AddForce = 2,
    AddImpulse = 3,
    AddTorque = 4,
    AddAngularImpulse = 5,
    Activate = 6,
    Deactivate = 7,
}

[StructLayout(LayoutKind.Sequential)]
public struct BodyState
{
    public Quaternion Rotation;
    public Vector3 Position;
    public uint IsActive;
    public Vector3 LinearVelocity;
    public Vector3 AngularVelocity;
}

public enum CharacterCommand : int
{
    SetLinearVelocity = 0,
    AddLinearVelocity = 1,
    AddImpulse = 2,
    SetPosition = 3,
}

[StructLayout(LayoutKind.Sequential)]
public struct CharacterState
{
    public Vector3 Position;
    public uint GroundState;
    public Vector3 LinearVelocity;
    public Vector3 GroundNormal;
}

[StructLayout(LayoutKind.Sequential)]
public struct FrameTime
{
    public double Elapsed;
    public float DeltaTime;
    public ulong Frame;
}

[StructLayout(LayoutKind.Sequential)]
public struct ScriptTypeInfo
{
    public StringView Name;
    public ScriptKind Kind;
}

// PhysicsContact in Scripting/SceneScript.hpp
[StructLayout(LayoutKind.Sequential)]
public struct PhysicsContact
{
    public ulong Entity1;
    public ulong Entity2;
    public Vector3 Point;
    public float PenetrationDepth;
    public Vector3 Normal;
    public ContactEvent Event;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeApi
{
    public uint Size;
    public uint IterSize;
    public ulong ChildOf;
    public FrameTime* Time;

    public delegate* unmanaged<LogLevel, byte*, nuint, void> Log;

    public delegate* unmanaged<nint, ulong> EcsNew;
    public delegate* unmanaged<nint, ulong, void> EcsDelete;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, bool> EcsIsAlive;
    public delegate* unmanaged<nint, ulong, ulong, void> EcsAddId;
    public delegate* unmanaged<nint, ulong, ulong, void> EcsRemoveId;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong, bool> EcsHasId;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong, void*> EcsGetMutId;
    public delegate* unmanaged<nint, ulong, ulong, nuint, void*, void> EcsSetId;
    public delegate* unmanaged<nint, ulong, ulong, void> EcsModifiedId;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong, int, ulong> EcsGetTarget;
    public delegate* unmanaged<nint, ulong, bool, void> EcsEnable;

    public delegate* unmanaged<nint, StringView, ulong> Lookup;
    public delegate* unmanaged<nint, ulong, StringView*, void> GetName;
    public delegate* unmanaged<nint, ulong, StringView, void> SetName;

    public delegate* unmanaged<nint, StringView, ComponentField*, int, ulong> DefineComponent;
    public delegate* unmanaged<nint, ComponentLayout*, int, ulong*, OxStatus> BindComponents;

    public delegate* unmanaged<nint, StringView, Phase, Term*, int, delegate* unmanaged<TableView*, void>, ulong> CreateSystem;
    public delegate* unmanaged<nint, Term*, int, void*> CreateQuery;
    public delegate* unmanaged<void*, void> DestroyQuery;
    public delegate* unmanaged<void*, void*, void> QueryIter;
    public delegate* unmanaged<void*, TableView*, void**, bool> QueryNext;
    public delegate* unmanaged<void*, void> IterFini;

    public delegate* unmanaged<nint, ulong, StringView, void> SetBehaviour;

    public delegate* unmanaged[SuppressGCTransition]<uint, InputState, bool> InputKey;
    public delegate* unmanaged[SuppressGCTransition]<uint, InputState, bool> InputMouseButton;
    public delegate* unmanaged[SuppressGCTransition]<MouseState*, void> InputMouse;
    public delegate* unmanaged[SuppressGCTransition]<ushort> InputModifiers;
    public delegate* unmanaged[SuppressGCTransition]<uint, int, InputState, bool> InputGamepadButton;
    public delegate* unmanaged[SuppressGCTransition]<uint, int, float> InputGamepadAxis;
    public delegate* unmanaged<uint*, int, int> InputGamepads;
    public delegate* unmanaged<StringView, uint, InputState, bool> InputAction;
    public delegate* unmanaged<StringView, uint, float*, void> InputActionAxis;
    public delegate* unmanaged<StringView, StringView, InputCodeData*, int, OxStatus> InputBindAction;
    public delegate* unmanaged<StringView, OxStatus> InputUnbindAction;
    public delegate* unmanaged<InputContextOp, StringView, void> InputContext;
    public delegate* unmanaged<float, float, void> InputWarpMouse;
    public delegate* unmanaged<bool, void> InputSetRelativeMouse;

    public delegate* unmanaged<nint, Ray*, RayHit*, bool> PhysicsRaycast;
    public delegate* unmanaged<nint, ulong, BodyState*, OxStatus> PhysicsBodyState;
    public delegate* unmanaged<nint, ulong, BodyCommand, Vector3*, OxStatus> PhysicsBodyCommand;
    public delegate* unmanaged<nint, ulong, CharacterState*, OxStatus> PhysicsCharacterState;
    public delegate* unmanaged<nint, ulong, CharacterCommand, Vector3*, OxStatus> PhysicsCharacterCommand;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ManagedApi
{
    public uint Size;
    public delegate* unmanaged<OxStatus> Shutdown;
    public delegate* unmanaged<nint, OxStatus> AttachWorld;
    public delegate* unmanaged<nint, OxStatus> DetachWorld;
    public delegate* unmanaged<ulong> AllocatedBytes;

    public delegate* unmanaged<StringView, ScriptKind> FindScriptType;
    public delegate* unmanaged<ScriptTypeInfo*, int, int> ListScriptTypes;
    public delegate* unmanaged<nint, StringView, ulong*, OxStatus> ScriptCreate;
    public delegate* unmanaged<ulong, void> ScriptDestroy;
    public delegate* unmanaged<nint, ulong, StringView, OxStatus> BehaviourAdd;
    public delegate* unmanaged<nint, ulong, void> BehaviourRemove;

    public delegate* unmanaged<nint, void> WorldStart;
    public delegate* unmanaged<nint, void> WorldStop;
    public delegate* unmanaged<nint, float, void> WorldUpdate;
    public delegate* unmanaged<nint, float, void> WorldFixedUpdate;
    public delegate* unmanaged<nint, uint, uint, void> WorldRender;
    public delegate* unmanaged<nint, PhysicsContact*, int, void> WorldContacts;
    public delegate* unmanaged<nint, ulong, bool, void> WorldBodyActivation;
}
