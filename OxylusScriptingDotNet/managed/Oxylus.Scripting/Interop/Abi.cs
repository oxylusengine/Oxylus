using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace Oxylus.Interop;

// mirrors OxylusScriptingDotNet/include/Scripting/DotNetAbi.hpp
public static class Abi
{
    public const uint Version = 11;
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
    SceneBehaviour = 1,
    EntityBehaviour = 2,
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
public struct RayData
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
    AddLinearVelocity = 8,
    AddForceAtPosition = 9,
    AddImpulseAtPosition = 10,
    MoveKinematic = 11,
    SetPosition = 12,
    SetRotation = 13,
    SetPositionAndRotation = 14,
}

[StructLayout(LayoutKind.Sequential)]
public struct BodyArgs
{
    public Vector3 Vector;
    public float DeltaTime;
    public Vector3 Position;
    public uint Padding;
    public Quaternion Rotation;
}

public enum BodyProperty : int
{
    MotionType = 0,
    ObjectLayer = 1,
    Friction = 2,
    Restitution = 3,
    IsSensor = 4,
    CollideKinematicVsNonDynamic = 5,
    UseManifoldReduction = 6,
    ApplyGyroscopicForce = 7,
    EnhancedInternalEdgeRemoval = 8,
}

[Flags]
public enum BodyFlags : uint
{
    IsSensor = 1 << 0,
    CollideKinematicVsNonDynamic = 1 << 1,
    UseManifoldReduction = 1 << 2,
    ApplyGyroscopicForce = 1 << 3,
    EnhancedInternalEdgeRemoval = 1 << 4,
}

[StructLayout(LayoutKind.Sequential)]
public struct BodyState
{
    public Quaternion Rotation;
    public Vector3 Position;
    public uint IsActive;
    public Vector3 LinearVelocity;
    public uint MotionType;
    public Vector3 AngularVelocity;
    public uint ObjectLayer;
    public Vector3 CenterOfMass;
    public float Friction;
    public Vector3 BoundsMin;
    public float Restitution;
    public Vector3 BoundsMax;
    public BodyFlags Flags;
    public Vector3 AccumulatedForce;
    public uint Padding0;
    public Vector3 AccumulatedTorque;
    public uint Padding1;
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
public struct VehicleState
{
    public float EngineRpm;
    public float EngineAngularVelocity;
    public int Gear;
    public float ForwardSpeed;
}

[StructLayout(LayoutKind.Sequential)]
public struct WheelState
{
    public uint IsContacting;
    public float LongitudinalSlip;
    public float LateralSlip;
}

[StructLayout(LayoutKind.Sequential)]
public struct AssetInfo
{
    public uint Type;
    public uint IsLoaded;
    public ulong RefCount;
}

public enum ParticleCommand : int
{
    Play = 0,
    Stop = 1,
    Restart = 2,
    EmitBurst = 3,
}

public enum CinematicCommand : int
{
    Play = 0,
    Stop = 1,
    Seek = 2,
}

[StructLayout(LayoutKind.Sequential)]
public struct CinematicState
{
    public float Time;
    public float Duration;
}

[StructLayout(LayoutKind.Sequential)]
public struct WindowSizeData
{
    public int PixelsX;
    public int PixelsY;
    public int LogicalX;
    public int LogicalY;
}

public enum VfsDir : int
{
    App = 0,
    Assets = 1,
    Cooked = 2,
}

public enum AudioCommand : int
{
    Play = 0,
    Stop = 1,
    Pause = 2,
    Resume = 3,
}

[StructLayout(LayoutKind.Sequential)]
public struct AudioState
{
    public uint IsPlaying;
    public float Cursor;
    public float Length;
    public uint Padding;
}

// OxValueType
public enum ValueKind : uint
{
    None = 0,
    Bool = 1,
    U8 = 2,
    U16 = 3,
    I32 = 4,
    I64 = 5,
    F32 = 6,
    F64 = 7,
    String = 8,
    Uuid = 9,
    Bytes = 10,
}

[StructLayout(LayoutKind.Sequential)]
public struct Value
{
    public ValueKind Kind;
    public uint Padding;
    public long Integer;
    public double Number;
    public StringView Text;
}

public enum UiDocumentCommand : int
{
    Show = 0,
    Hide = 1,
    Close = 2,
}

[StructLayout(LayoutKind.Sequential)]
public struct UiEventData
{
    public StringView Type;
    public ulong Target;
    public ulong Current;
    public Vector2 Mouse;
}

public enum NetEvent : int
{
    ClientConnected = 0,
    ClientDisconnected = 1,
    Connected = 2,
    Disconnected = 3,
    ClientAcked = 4,
}

[StructLayout(LayoutKind.Sequential)]
public struct NetClientState
{
    public uint Status;
    public uint Ping;
    public ulong NetId;
    public uint Rtt;
    public uint PacketsLost;
    public uint SentBytes;
    public uint ReceivedBytes;
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
    public ulong IsA;
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
    public delegate* unmanaged<nint, ulong, ulong, nuint, void*> EcsEnsureId;
    public delegate* unmanaged<nint, bool> EcsDeferBegin;
    public delegate* unmanaged<nint, bool> EcsDeferEnd;
    public delegate* unmanaged<nint, void> EcsDeferSuspend;
    public delegate* unmanaged<nint, void> EcsDeferResume;
    public delegate* unmanaged<nint, ulong, byte*, uint, uint> GetPath;
    public delegate* unmanaged<nint, ulong, void*, void> ChildrenIter;
    public delegate* unmanaged<void*, ulong**, int*, bool> ChildrenNext;

    public delegate* unmanaged<nint, StringView, ulong> Lookup;
    public delegate* unmanaged<nint, ulong, StringView*, void> GetName;
    public delegate* unmanaged<nint, ulong, StringView, void> SetName;

    public delegate* unmanaged<nint, StringView, ComponentField*, int, ulong> DefineComponent;
    public delegate* unmanaged<nint, ulong, void*, uint, OxStatus> SetComponentDefaults;
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

    public delegate* unmanaged<nint, RayData*, RayHit*, bool> PhysicsRaycast;
    public delegate* unmanaged<nint, ulong, BodyState*, OxStatus> PhysicsBodyState;
    public delegate* unmanaged<nint, ulong, BodyCommand, BodyArgs*, OxStatus> PhysicsBodyCommand;
    public delegate* unmanaged<nint, ulong, BodyProperty, float, OxStatus> PhysicsBodySet;
    public delegate* unmanaged<nint, ulong, CharacterState*, OxStatus> PhysicsCharacterState;
    public delegate* unmanaged<nint, ulong, CharacterCommand, Vector3*, OxStatus> PhysicsCharacterCommand;
    public delegate* unmanaged<nint, ulong, VehicleState*, OxStatus> PhysicsVehicleState;
    public delegate* unmanaged<nint, ulong, WheelState*, OxStatus> PhysicsWheelState;
    public delegate* unmanaged<nint, ulong, Vector2*, Vector2*, RayData*, OxStatus> PhysicsScreenRay;

    public delegate* unmanaged<StringView, AssetId*, bool> AssetFind;
    public delegate* unmanaged<AssetId*, bool> AssetAcquire;
    public delegate* unmanaged<AssetId*, void> AssetRelease;
    public delegate* unmanaged<AssetId*, AssetInfo*, bool> AssetInfo;
    public delegate* unmanaged<AssetId*, AssetId*, int, int*, OxStatus> AssetMaterials;
    public delegate* unmanaged<AssetId*, MaterialData*, OxStatus> AssetMaterialGet;
    public delegate* unmanaged<AssetId*, MaterialData*, OxStatus> AssetMaterialSet;

    public delegate* unmanaged<nint, ulong, AudioCommand, OxStatus> AudioCommand;
    public delegate* unmanaged<nint, ulong, AudioState*, OxStatus> AudioState;
    public delegate* unmanaged<nint, ulong, AssetId*, OxStatus> AudioSetSound;
    public delegate* unmanaged<AssetId*, float, Vector3*, OxStatus> AudioOneShot;

    public delegate* unmanaged<nint, StringView, ulong, ulong> SceneSpawn;
    public delegate* unmanaged<nint, AssetId*, ulong, ulong> SceneSpawnModel;
    public delegate* unmanaged<nint, AssetId*, ulong> SceneSpawnParticles;
    public delegate* unmanaged<nint, ulong, ParticleCommand, uint, void> SceneParticlesCommand;
    public delegate* unmanaged<nint, ulong, bool> SceneParticlesPlaying;
    public delegate* unmanaged<nint, ulong, uint, Vector4*, void> SceneParticlesSetParameter;
    public delegate* unmanaged<nint, ulong, StringView, Vector4*, bool> SceneParticlesSetParameterNamed;
    public delegate* unmanaged<nint, ulong, CinematicCommand, float, void> SceneCinematicCommand;
    public delegate* unmanaged<nint, ulong, CinematicState*, void> SceneCinematicState;
    public delegate* unmanaged<nint, bool> SceneInputFocused;
    public delegate* unmanaged<nint, AssetId*, bool> SceneUuid;
    public delegate* unmanaged<nint, StringView, OxStatus> SceneSave;
    public delegate* unmanaged<nint, float, void> SceneSetUiDpiRatio;
    public delegate* unmanaged<nint, void> SceneClearUiDpiRatio;
    public delegate* unmanaged<AssetId*, OxStatus> SceneRequestLoad;

    public delegate* unmanaged<VfsDir, StringView, byte*, uint, uint> VfsResolve;
    public delegate* unmanaged<StringView, byte*, uint, uint> VfsToPhysical;
    public delegate* unmanaged<StringView, byte*, uint, uint> VfsToVirtual;

    public delegate* unmanaged<nint, StringView, ulong*, OxStatus> UiLoadDocument;
    public delegate* unmanaged<StringView, bool, OxStatus> UiLoadFontFace;
    public delegate* unmanaged<ulong, UiDocumentCommand, OxStatus> UiDocument;
    public delegate* unmanaged<ulong, StringView, ulong*, OxStatus> UiQuery;
    public delegate* unmanaged<ulong, StringView, OxStatus> UiSetInnerRml;
    public delegate* unmanaged<ulong, byte*, uint, uint*, OxStatus> UiGetInnerRml;
    public delegate* unmanaged<ulong, StringView, StringView, OxStatus> UiSetAttribute;
    public delegate* unmanaged<ulong, StringView, byte*, uint, uint*, OxStatus> UiGetAttribute;
    public delegate* unmanaged<ulong, StringView, StringView, OxStatus> UiSetProperty;
    public delegate* unmanaged<ulong, StringView, bool, OxStatus> UiSetClass;
    public delegate* unmanaged<ulong, StringView, ulong*, OxStatus> UiAddListener;
    public delegate* unmanaged<ulong, void> UiRemoveListener;
    public delegate* unmanaged<nint, StringView, ulong*, OxStatus> UiModelCreate;
    public delegate* unmanaged<ulong, StringView, int, bool, OxStatus> UiModelBind;
    public delegate* unmanaged<ulong, StringView, int, OxStatus> UiModelBindEvent;
    public delegate* unmanaged<ulong, StringView, void> UiModelDirty;
    public delegate* unmanaged<ulong, void> UiModelRemove;
    public delegate* unmanaged<void> UiClearStyleSheetCache;
    public delegate* unmanaged<void> UiClearTemplateCache;

    public delegate* unmanaged<WindowSizeData*, void> WindowSize;
    public delegate* unmanaged<double> TimeMaxFrameTime;
    public delegate* unmanaged<double, void> TimeSetMaxFrameTime;
    public delegate* unmanaged<void> TimeResetMaxFrameTime;
    public delegate* unmanaged<void> AppQuit;

    public delegate* unmanaged<ushort, uint, ulong*, OxStatus> NetServerStart;
    public delegate* unmanaged<StringView, ushort, uint, ulong*, OxStatus> NetClientConnect;
    public delegate* unmanaged<ulong, void> NetClose;
    public delegate* unmanaged<ulong, NetClientState*, OxStatus> NetClientState;
    public delegate* unmanaged<ulong, ulong*, int, int> NetServerClients;
    public delegate* unmanaged<ulong, StringView, int, OxStatus> NetRegisterRpc;
    public delegate* unmanaged<ulong, ulong, StringView, Value*, int, bool, OxStatus> NetCall;
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

    public delegate* unmanaged<ulong, UiEventData*, void> UiEvent;
    public delegate* unmanaged<ulong, int, Value*, void> UiModelGet;
    public delegate* unmanaged<ulong, int, Value*, void> UiModelSet;
    public delegate* unmanaged<ulong, int, UiEventData*, Value*, int, void> UiModelEvent;
    public delegate* unmanaged<ulong, int, ulong, Value*, int, void> NetRpc;
    public delegate* unmanaged<ulong, NetEvent, ulong, uint, void> NetEvent;
    public delegate* unmanaged<ulong, byte, ulong*, int, ulong*, int, void> NetSnapshot;
    public delegate* unmanaged<uint, uint, void> WindowResized;
}
