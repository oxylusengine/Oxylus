using System;
using System.Numerics;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// the app's keyboard, mouse and gamepads. Pressed and Released hold for the one frame the change happened in, Held
// for as long as it lasts
public static unsafe class Input
{
    public static bool IsKeyHeld(ScanCode key) => Runtime.Native.InputKey((uint)key, InputState.Held);
    public static bool IsKeyPressed(ScanCode key) => Runtime.Native.InputKey((uint)key, InputState.Pressed);
    public static bool IsKeyReleased(ScanCode key) => Runtime.Native.InputKey((uint)key, InputState.Released);

    public static bool IsMouseHeld(MouseCode button) => Runtime.Native.InputMouseButton((uint)button, InputState.Held);
    public static bool IsMousePressed(MouseCode button) => Runtime.Native.InputMouseButton((uint)button, InputState.Pressed);
    public static bool IsMouseReleased(MouseCode button) => Runtime.Native.InputMouseButton((uint)button, InputState.Released);

    // window coordinates
    public static Vector2 MousePosition => Mouse.Position;
    // how far the mouse moved this frame
    public static Vector2 MouseDelta => Mouse.Delta;
    public static float ScrollDelta => Mouse.Scroll;
    public static bool IsMouseMoved => Mouse.IsMoved != 0;

    public static ModCode Modifiers => (ModCode)Runtime.Native.InputModifiers();

    static MouseState Mouse
    {
        get
        {
            MouseState state;
            Runtime.Native.InputMouse(&state);
            return state;
        }
    }

    public static void WarpMouse(Vector2 windowPosition) => Runtime.Native.InputWarpMouse(windowPosition.X, windowPosition.Y);

    // the cursor hides and the mouse reports only motion, what a first person camera wants
    public static void SetRelativeMouse(bool isEnabled) => Runtime.Native.InputSetRelativeMouse(isEnabled);

    // fills `gamepads` with the connected ones and returns how many there are, which can be more than fit
    public static int GetGamepads(Span<Gamepad> gamepads)
    {
        Span<uint> ids = stackalloc uint[gamepads.Length];
        int count;
        fixed (uint* data = ids)
        {
            count = Runtime.Native.InputGamepads(data, ids.Length);
        }

        for (var i = 0; i < Math.Min(count, gamepads.Length); i++)
        {
            gamepads[i] = new Gamepad(ids[i]);
        }

        return count;
    }

    // an action is only live while its context is the active one, "default" unless a script or the game changes it
    public static void SetContext(string context) => Context(InputContextOp.Set, context);
    public static void PushContext(string context) => Context(InputContextOp.Push, context);
    public static void PopContext() => Context(InputContextOp.Pop, "");

    public static void BindAction(string action, params ReadOnlySpan<InputBinding> inputs) => BindAction(action, "default", inputs);

    public static void BindAction(string action, string context, params ReadOnlySpan<InputBinding> inputs)
    {
        var actionBytes = Encoding.UTF8.GetBytes(action);
        var contextBytes = Encoding.UTF8.GetBytes(context);
        Span<InputCodeData> codes = stackalloc InputCodeData[inputs.Length];
        for (var i = 0; i < inputs.Length; i++)
        {
            codes[i] = inputs[i].Data;
        }

        OxStatus status;
        fixed (byte* actionData = actionBytes)
        fixed (byte* contextData = contextBytes)
        fixed (InputCodeData* codeData = codes)
        {
            status = Runtime.Native.InputBindAction(
                new StringView(actionData, actionBytes.Length),
                new StringView(contextData, contextBytes.Length),
                codeData,
                codes.Length);
        }

        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException($"Couldn't bind input action {action}: {status}");
        }
    }

    public static bool UnbindAction(string action)
    {
        var bytes = Encoding.UTF8.GetBytes(action);
        fixed (byte* data = bytes)
        {
            return Runtime.Native.InputUnbindAction(new StringView(data, bytes.Length)) == OxStatus.Ok;
        }
    }

    static void Context(InputContextOp op, string context)
    {
        var bytes = Encoding.UTF8.GetBytes(context);
        fixed (byte* data = bytes)
        {
            Runtime.Native.InputContext(op, new StringView(data, bytes.Length));
        }
    }
}

// an action bound in the engine, by name. Make one once and keep it: the name is encoded a single time, where a call
// that took a string would encode it on every query
public readonly unsafe struct InputAction(string name)
{
    readonly byte[] name = Encoding.UTF8.GetBytes(name);

    public bool IsHeld(Gamepad gamepad = default) => Query(InputState.Held, gamepad);
    public bool IsPressed(Gamepad gamepad = default) => Query(InputState.Pressed, gamepad);
    public bool IsReleased(Gamepad gamepad = default) => Query(InputState.Released, gamepad);

    public Vector2 Axis(Gamepad gamepad = default)
    {
        var axis = Vector2.Zero;
        fixed (byte* data = name)
        {
            Runtime.Native.InputActionAxis(new StringView(data, name.Length), gamepad.Id, (float*)&axis);
        }

        return axis;
    }

    bool Query(InputState state, Gamepad gamepad)
    {
        fixed (byte* data = name)
        {
            return Runtime.Native.InputAction(new StringView(data, name.Length), gamepad.Id, state);
        }
    }
}

// a connected gamepad by its SDL instance id, from Input.GetGamepads
public readonly struct Gamepad(uint id) : IEquatable<Gamepad>
{
    public uint Id { get; } = id;

    public unsafe bool IsHeld(GamepadButtonCode button) => Runtime.Native.InputGamepadButton(Id, (int)button, InputState.Held);
    public unsafe bool IsPressed(GamepadButtonCode button) => Runtime.Native.InputGamepadButton(Id, (int)button, InputState.Pressed);
    public unsafe bool IsReleased(GamepadButtonCode button) => Runtime.Native.InputGamepadButton(Id, (int)button, InputState.Released);

    // -1 to 1, triggers 0 to 1
    public unsafe float Axis(GamepadAxisCode axis) => Runtime.Native.InputGamepadAxis(Id, (int)axis);

    public bool Equals(Gamepad other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is Gamepad other && Equals(other);
    public override int GetHashCode() => (int)Id;
}

public enum AxisDirection : byte
{
    Positive = 0,
    Negative = 1,
    Both = 2,
}

// one input an action listens to, made with the factories below
public readonly struct InputBinding
{
    // Core/Input.hpp's InputType
    const byte Keyboard = 1;
    const byte MouseButton = 2;
    const byte MouseAxisType = 3;
    const byte GamepadButtonType = 4;
    const byte GamepadAxisType = 5;

    internal InputCodeData Data { get; }

    InputBinding(byte type, int code, ModCode mods = ModCode.None, AxisDirection direction = AxisDirection.Both) =>
        Data = new InputCodeData { Type = type, Code = code, Mod = (ushort)mods, AxisDirection = (byte)direction };

    public static InputBinding Key(ScanCode key, ModCode mods = ModCode.None) => new(Keyboard, (int)key, mods);
    public static InputBinding Mouse(MouseCode button, ModCode mods = ModCode.None) => new(MouseButton, (int)button, mods);
    public static InputBinding MouseAxis(MouseAxisCode axis) => new(MouseAxisType, (int)axis);
    public static InputBinding GamepadButton(GamepadButtonCode button) => new(GamepadButtonType, (int)button);

    public static InputBinding GamepadAxis(GamepadAxisCode axis, AxisDirection direction = AxisDirection.Both) =>
        new(GamepadAxisType, (int)axis, ModCode.None, direction);
}
