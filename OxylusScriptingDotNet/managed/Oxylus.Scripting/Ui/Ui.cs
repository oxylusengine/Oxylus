using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// RmlUi in a scene, the game's UI. Documents and elements are handles: once RmlUi drops an element, calls on it throw
// rather than touch freed memory. ImGui isn't offered, a shipped game never runs an ImGui frame
public static unsafe class Ui
{
    internal static readonly Dictionary<ulong, Action<UiEvent>> Listeners = [];
    internal static readonly Dictionary<ulong, UiDataModel> Models = [];

    // `path` is relative to the game's assets, "UI/hud.rml"
    public static UiDocument LoadDocument(World world, string path)
    {
        var bytes = Encoding.UTF8.GetBytes(path);
        ulong document;
        fixed (byte* data = bytes)
        {
            Check(
                Runtime.Native.UiLoadDocument(world.Handle, new StringView(data, bytes.Length), &document),
                $"Couldn't load {path}, does the world's scene have a UI and the file exist?");
        }

        return new UiDocument(document);
    }

    // `path` is relative to the game's assets, "Fonts/FiraSans-Regular.ttf". A fallback face covers glyphs the other
    // faces lack
    public static void LoadFontFace(string path, bool isFallback = false)
    {
        var bytes = Encoding.UTF8.GetBytes(path);
        fixed (byte* data = bytes)
        {
            Check(
                Runtime.Native.UiLoadFontFace(new StringView(data, bytes.Length), isFallback),
                $"Couldn't load font face {path}, does the file exist and is it a font?");
        }
    }

    // a data model the documents loaded after it can bind to with data-model="name"
    public static UiDataModel CreateDataModel(World world, string name)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        ulong model;
        fixed (byte* data = bytes)
        {
            Check(
                Runtime.Native.UiModelCreate(world.Handle, new StringView(data, bytes.Length), &model),
                $"Couldn't create data model {name}, is the name taken?");
        }

        var created = new UiDataModel(model);
        Models[model] = created;
        return created;
    }

    // makes the next document load read its style sheets and templates from disk again
    public static void ClearStyleSheetCache() => Runtime.Native.UiClearStyleSheetCache();

    public static void ClearTemplateCache() => Runtime.Native.UiClearTemplateCache();

    internal static void Check(OxStatus status, string message)
    {
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException(message);
        }
    }

    // inner RML when `name` is null, otherwise that attribute
    internal static string ReadText(ulong element, string? name)
    {
        var nameBytes = name is null ? [] : Encoding.UTF8.GetBytes(name);
        fixed (byte* nameData = nameBytes)
        {
            var view = new StringView(nameData, nameBytes.Length);
            var isInnerRml = name is null;
            Span<byte> buffer = stackalloc byte[256];
            uint length;
            fixed (byte* data = buffer)
            {
                Check(Read(element, isInnerRml, view, data, (uint)buffer.Length, &length), "The element is gone");
            }

            if (length <= buffer.Length)
            {
                return Encoding.UTF8.GetString(buffer[..(int)length]);
            }

            var large = new byte[length];
            fixed (byte* data = large)
            {
                Check(Read(element, isInnerRml, view, data, length, &length), "The element is gone");
            }

            return Encoding.UTF8.GetString(large);
        }
    }

    static OxStatus Read(ulong element, bool isInnerRml, StringView name, byte* buffer, uint capacity, uint* length) =>
        isInnerRml
            ? Runtime.Native.UiGetInnerRml(element, buffer, capacity, length)
            : Runtime.Native.UiGetAttribute(element, name, buffer, capacity, length);

    internal static void Release()
    {
        Listeners.Clear();
        foreach (var model in Models.Values)
        {
            model.Detach();
        }

        Models.Clear();
    }
}

public readonly unsafe struct UiElement(ulong handle) : IEquatable<UiElement>
{
    internal ulong Handle { get; } = handle;

    public bool IsValid => Handle != 0;

    // a CSS selector under this element, default when nothing matches
    public UiElement Query(string selector)
    {
        var bytes = Encoding.UTF8.GetBytes(selector);
        ulong found;
        fixed (byte* data = bytes)
        {
            Runtime.Native.UiQuery(Handle, new StringView(data, bytes.Length), &found);
        }

        return new UiElement(found);
    }

    public UiElement GetElementById(string id) => Query($"#{id}");

    public string InnerRml => Ui.ReadText(Handle, null);

    public void SetInnerRml(string rml)
    {
        var bytes = Encoding.UTF8.GetBytes(rml);
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiSetInnerRml(Handle, new StringView(data, bytes.Length)), "The element is gone");
        }
    }

    public string GetAttribute(string name) => Ui.ReadText(Handle, name);

    public void SetAttribute(string name, string value) => Pair(name, value, isProperty: false);

    // an RCSS property, "color" to "red"
    public void SetProperty(string name, string value) => Pair(name, value, isProperty: true);

    public void SetClass(string name, bool isActive)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiSetClass(Handle, new StringView(data, bytes.Length), isActive), "The element is gone");
        }
    }

    // until the listener is disposed or the scripts unload
    public UiListener AddEventListener(string eventType, Action<UiEvent> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        var bytes = Encoding.UTF8.GetBytes(eventType);
        ulong listener;
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiAddListener(Handle, new StringView(data, bytes.Length), &listener), "The element is gone");
        }

        Ui.Listeners[listener] = handler;
        return new UiListener(listener);
    }

    void Pair(string first, string second, bool isProperty)
    {
        var firstBytes = Encoding.UTF8.GetBytes(first);
        var secondBytes = Encoding.UTF8.GetBytes(second);
        fixed (byte* a = firstBytes)
        fixed (byte* b = secondBytes)
        {
            var name = new StringView(a, firstBytes.Length);
            var value = new StringView(b, secondBytes.Length);
            var status = isProperty
                ? Runtime.Native.UiSetProperty(Handle, name, value)
                : Runtime.Native.UiSetAttribute(Handle, name, value);
            Ui.Check(status, "The element is gone or refused the value");
        }
    }

    public bool Equals(UiElement other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is UiElement other && Equals(other);
    public override int GetHashCode() => Handle.GetHashCode();
}

public readonly unsafe struct UiDocument(ulong handle)
{
    public UiElement Root { get; } = new(handle);

    public void Show() => Command(UiDocumentCommand.Show);
    public void Hide() => Command(UiDocumentCommand.Hide);
    public void Close() => Command(UiDocumentCommand.Close);

    public UiElement Query(string selector) => Root.Query(selector);
    public UiElement GetElementById(string id) => Root.GetElementById(id);

    public static implicit operator UiElement(UiDocument document) => document.Root;

    void Command(UiDocumentCommand command) => Ui.Check(Runtime.Native.UiDocument(Root.Handle, command), "The document is gone");
}

public readonly unsafe ref struct UiEvent
{
    readonly StringView type;

    internal UiEvent(UiEventData* data)
    {
        type = data->Type;
        Target = new UiElement(data->Target);
        Current = new UiElement(data->Current);
        Mouse = data->Mouse;
    }

    public UiElement Target { get; }
    // the element the listener is on
    public UiElement Current { get; }
    public Vector2 Mouse { get; }

    public string Type => Encoding.UTF8.GetString(type.Data, (int)type.Length);

    // `e.Is("click"u8)` compares without making a string
    public bool Is(ReadOnlySpan<byte> utf8) => new ReadOnlySpan<byte>(type.Data, (int)type.Length).SequenceEqual(utf8);
}

public readonly unsafe struct UiListener(ulong id) : IDisposable
{
    public void Dispose()
    {
        if (Ui.Listeners.Remove(id))
        {
            Runtime.Native.UiRemoveListener(id);
        }
    }
}

// a data event's arguments, `data-event-click="pick(3, 'red')"` hands over 3 and "red"
public readonly unsafe ref struct UiArgs(Value* values, int count)
{
    public int Count { get; } = count;

    public long GetInteger(int index) => At(index).Kind is ValueKind.F64 or ValueKind.F32 ? (long)At(index).Number : At(index).Integer;

    public double GetNumber(int index) => At(index).Kind is ValueKind.F64 or ValueKind.F32 ? At(index).Number : At(index).Integer;

    public bool GetBool(int index) => At(index).Integer != 0;

    public string GetString(int index)
    {
        ref readonly var value = ref At(index);
        return value.Kind switch
        {
            ValueKind.String => Encoding.UTF8.GetString(value.Text.Data, (int)value.Text.Length),
            ValueKind.F64 or ValueKind.F32 => value.Number.ToString(System.Globalization.CultureInfo.InvariantCulture),
            _ => value.Integer.ToString(System.Globalization.CultureInfo.InvariantCulture),
        };
    }

    ref readonly Value At(int index)
    {
        if ((uint)index >= (uint)Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }

        return ref values[index];
    }
}

public delegate void UiDataEvent(UiEvent e, UiArgs args);

// variables are read through their getter whenever RmlUi updates a binding, call Dirty after changing what a getter
// returns so it does
public sealed unsafe class UiDataModel : IDisposable
{
    readonly List<UiBinding> bindings = [];
    readonly List<UiDataEvent> events = [];
    // a getter's string, pinned so native code can copy it after the call returns
    byte[] text = GC.AllocateArray<byte>(256, pinned: true);
    ulong handle;

    internal UiDataModel(ulong handle) => this.handle = handle;

    public void Bind<T>(string name, Func<T> get, Action<T>? set = null)
    {
        ArgumentNullException.ThrowIfNull(get);
        var binding = UiBinding.Create(get, set);
        var bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* data = bytes)
        {
            Ui.Check(
                Runtime.Native.UiModelBind(Live, new StringView(data, bytes.Length), bindings.Count, set is not null),
                $"Couldn't bind {name}, is it already bound or the model in use?");
        }

        bindings.Add(binding);
    }

    public void BindEvent(string name, UiDataEvent handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        var bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiModelBindEvent(Live, new StringView(data, bytes.Length), events.Count), $"Couldn't bind event {name}");
        }

        events.Add(handler);
    }

    public void Dirty(string name)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* data = bytes)
        {
            Runtime.Native.UiModelDirty(Live, new StringView(data, bytes.Length));
        }
    }

    public void DirtyAll() => Runtime.Native.UiModelDirty(Live, default);

    public void Dispose()
    {
        if (handle == 0)
        {
            return;
        }

        Ui.Models.Remove(handle);
        Runtime.Native.UiModelRemove(handle);
        handle = 0;
    }

    internal void Detach() => handle = 0;

    ulong Live => handle != 0 ? handle : throw new ObjectDisposedException(nameof(UiDataModel));

    internal void Get(int index, Value* value)
    {
        if ((uint)index < (uint)bindings.Count)
        {
            bindings[index].Get(value, ref text);
        }
    }

    internal void Set(int index, Value* value)
    {
        if ((uint)index < (uint)bindings.Count)
        {
            bindings[index].Set(value);
        }
    }

    internal void Event(int index, UiEventData* data, Value* args, int count)
    {
        if ((uint)index < (uint)events.Count)
        {
            events[index](new UiEvent(data), new UiArgs(args, count));
        }
    }
}

internal abstract unsafe class UiBinding
{
    public abstract void Get(Value* value, ref byte[] text);
    public abstract void Set(Value* value);

    public static UiBinding Create<T>(Func<T> get, Action<T>? set)
    {
        if (typeof(T) != typeof(bool) && typeof(T) != typeof(int) && typeof(T) != typeof(long) &&
            typeof(T) != typeof(float) && typeof(T) != typeof(double) && typeof(T) != typeof(string))
        {
            throw new NotSupportedException($"A data model variable is a bool, int, long, float, double or string, not {typeof(T).Name}");
        }

        return new UiBinding<T>(get, set);
    }
}

internal sealed unsafe class UiBinding<T>(Func<T> get, Action<T>? set) : UiBinding
{
    public override void Get(Value* value, ref byte[] text)
    {
        var current = get();
        *value = default;
        if (typeof(T) == typeof(bool))
        {
            value->Kind = ValueKind.Bool;
            value->Integer = Unsafe.As<T, bool>(ref current) ? 1 : 0;
        }
        else if (typeof(T) == typeof(int))
        {
            value->Kind = ValueKind.I64;
            value->Integer = Unsafe.As<T, int>(ref current);
        }
        else if (typeof(T) == typeof(long))
        {
            value->Kind = ValueKind.I64;
            value->Integer = Unsafe.As<T, long>(ref current);
        }
        else if (typeof(T) == typeof(float))
        {
            value->Kind = ValueKind.F64;
            value->Number = Unsafe.As<T, float>(ref current);
        }
        else if (typeof(T) == typeof(double))
        {
            value->Kind = ValueKind.F64;
            value->Number = Unsafe.As<T, double>(ref current);
        }
        else
        {
            var str = (string?)(object?)current ?? "";
            var length = Encoding.UTF8.GetByteCount(str);
            if (length > text.Length)
            {
                text = GC.AllocateArray<byte>(Math.Max(length, text.Length * 2), pinned: true);
            }

            Encoding.UTF8.GetBytes(str, text);
            value->Kind = ValueKind.String;
            value->Text = new StringView((byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(text)), length);
        }
    }

    // a write from the UI, an input box or a slider, which happens at human speed
    public override void Set(Value* value)
    {
        if (set is null)
        {
            return;
        }

        var isNumber = value->Kind is ValueKind.F64 or ValueKind.F32;
        object converted;
        if (typeof(T) == typeof(bool))
        {
            converted = value->Integer != 0;
        }
        else if (typeof(T) == typeof(int))
        {
            converted = isNumber ? (int)value->Number : (int)value->Integer;
        }
        else if (typeof(T) == typeof(long))
        {
            converted = isNumber ? (long)value->Number : value->Integer;
        }
        else if (typeof(T) == typeof(float))
        {
            converted = isNumber ? (float)value->Number : value->Integer;
        }
        else if (typeof(T) == typeof(double))
        {
            converted = isNumber ? value->Number : value->Integer;
        }
        else
        {
            converted = value->Kind == ValueKind.String ? Encoding.UTF8.GetString(value->Text.Data, (int)value->Text.Length) : "";
        }

        set((T)converted);
    }
}
