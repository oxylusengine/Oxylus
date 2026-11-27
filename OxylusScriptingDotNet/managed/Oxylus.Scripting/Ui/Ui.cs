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

    // inner RML for `TextKind.InnerRml`, otherwise the named attribute or property
    internal static string ReadText(ulong element, TextKind kind, string name = "")
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var message = kind == TextKind.Property ? "The element is gone or has no such property" : "The element is gone";
        fixed (byte* nameData = nameBytes)
        {
            var view = new StringView(nameData, nameBytes.Length);
            Span<byte> buffer = stackalloc byte[256];
            uint length;
            fixed (byte* data = buffer)
            {
                Check(Read(element, kind, view, data, (uint)buffer.Length, &length), message);
            }

            if (length <= buffer.Length)
            {
                return Encoding.UTF8.GetString(buffer[..(int)length]);
            }

            var large = new byte[length];
            fixed (byte* data = large)
            {
                Check(Read(element, kind, view, data, length, &length), message);
            }

            return Encoding.UTF8.GetString(large);
        }
    }

    static OxStatus Read(ulong element, TextKind kind, StringView name, byte* buffer, uint capacity, uint* length) => kind switch
    {
        TextKind.InnerRml => Runtime.Native.UiGetInnerRml(element, buffer, capacity, length),
        TextKind.Attribute => Runtime.Native.UiGetAttribute(element, name, buffer, capacity, length),
        _ => Runtime.Native.UiGetProperty(element, name, buffer, capacity, length),
    };

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

internal enum TextKind
{
    InnerRml,
    Attribute,
    Property,
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

    public string InnerRml => Ui.ReadText(Handle, TextKind.InnerRml);

    public void SetInnerRml(string rml)
    {
        var bytes = Encoding.UTF8.GetBytes(rml);
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiSetInnerRml(Handle, new StringView(data, bytes.Length)), "The element is gone");
        }
    }

    public string GetAttribute(string name) => Ui.ReadText(Handle, TextKind.Attribute, name);

    // what RmlUi resolved for an RCSS property, "display" gives "block", and throws when the element has none
    public string GetProperty(string name) => Ui.ReadText(Handle, TextKind.Property, name);

    public bool HasClass(string name)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        bool isSet;
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiHasClass(Handle, new StringView(data, bytes.Length), &isSet), "The element is gone");
        }

        return isSet;
    }

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

    // false when RmlUi won't focus it, an element has to be focusable
    public bool Focus()
    {
        var status = Runtime.Native.UiElementCommand(Handle, UiElementCommand.Focus);
        if (status == OxStatus.InvalidArgument)
        {
            return false;
        }

        Ui.Check(status, "The element is gone");
        return true;
    }

    public void Blur() => Command(UiElementCommand.Blur);

    public void Click() => Command(UiElementCommand.Click);

    public void ScrollIntoView() => Command(UiElementCommand.ScrollIntoView);

    // the element and its children are gone, calls on them throw from then on. A document closes through
    // UiDocument.Close and has no parent to leave
    public void Remove() => Command(UiElementCommand.Remove);

    // a new element with the tag, `AppendChild("div")`, at the end of this one's children
    public UiElement AppendChild(string tag)
    {
        var bytes = Encoding.UTF8.GetBytes(tag);
        ulong child;
        fixed (byte* data = bytes)
        {
            Ui.Check(Runtime.Native.UiAppendChild(Handle, new StringView(data, bytes.Length), &child), $"Couldn't append {tag}, is the element gone?");
        }

        return new UiElement(child);
    }

    // every match under this element, empty when nothing matches
    public UiElement[] QueryAll(string selector)
    {
        var bytes = Encoding.UTF8.GetBytes(selector);
        Span<ulong> first = stackalloc ulong[8];
        int count;
        fixed (byte* data = bytes)
        fixed (ulong* found = first)
        {
            var view = new StringView(data, bytes.Length);
            Ui.Check(Runtime.Native.UiQueryAll(Handle, view, found, first.Length, &count), "The element is gone");
            if (count <= first.Length)
            {
                return Wrap(first[..count]);
            }

            var large = new ulong[count];
            fixed (ulong* all = large)
            {
                Ui.Check(Runtime.Native.UiQueryAll(Handle, view, all, large.Length, &count), "The element is gone");
            }

            return Wrap(large.AsSpan(0, Math.Min(count, large.Length)));
        }
    }

    // default when this is a document
    public UiElement Parent
    {
        get
        {
            ulong parent;
            Ui.Check(Runtime.Native.UiParent(Handle, &parent), "The element is gone");
            return new UiElement(parent);
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

    static UiElement[] Wrap(ReadOnlySpan<ulong> handles)
    {
        var elements = new UiElement[handles.Length];
        for (var i = 0; i < elements.Length; i++)
        {
            elements[i] = new UiElement(handles[i]);
        }

        return elements;
    }

    void Command(UiElementCommand command) => Ui.Check(
        Runtime.Native.UiElementCommand(Handle, command),
        command == UiElementCommand.Remove ? "The element is gone, a document or has no parent" : "The element is gone");

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
    readonly nint native;

    internal UiEvent(UiEventData* data)
    {
        type = data->Type;
        native = data->Event;
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

    // RmlUi's event parameters by name, the fallback when the event has no such one
    public long GetInteger(string name, long fallback = 0) => TryGet(name, out var value) ? UiValues.Integer(value) : fallback;

    public double GetNumber(string name, double fallback = 0) => TryGet(name, out var value) ? UiValues.Number(value) : fallback;

    public bool GetBool(string name) => TryGet(name, out var value) && value.Integer != 0;

    public string GetString(string name, string fallback = "") => TryGet(name, out var value) ? UiValues.Text(value) : fallback;

    // key events
    public UiKey Key => (UiKey)GetInteger("key_identifier");
    public bool Ctrl => GetBool("ctrl_key");
    public bool Shift => GetBool("shift_key");
    public bool Alt => GetBool("alt_key");
    public bool Meta => GetBool("meta_key");

    // mouse events, the button's index, 0 for the left one
    public int Button => (int)GetInteger("button");
    public Vector2 Wheel => new((float)GetNumber("wheel_delta_x"), (float)GetNumber("wheel_delta_y"));

    // later elements on the path don't see the event, the listeners left on this one still run
    public void StopPropagation()
    {
        if (native != 0)
        {
            Runtime.Native.UiEventStop(native, false);
        }
    }

    // not even the listeners left on this element see it
    public void StopImmediatePropagation()
    {
        if (native != 0)
        {
            Runtime.Native.UiEventStop(native, true);
        }
    }

    bool TryGet(string name, out Value value)
    {
        value = default;
        if (native == 0)
        {
            return false;
        }

        var bytes = Encoding.UTF8.GetBytes(name);
        Value found = default;
        OxStatus status;
        fixed (byte* data = bytes)
        {
            status = Runtime.Native.UiEventParameter(native, new StringView(data, bytes.Length), &found);
        }

        // a string is only valid until the next call, so the caller reads it before making one
        value = found;
        return status == OxStatus.Ok;
    }
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

    public long GetInteger(int index) => UiValues.Integer(At(index));

    public double GetNumber(int index) => UiValues.Number(At(index));

    public bool GetBool(int index) => At(index).Integer != 0;

    public string GetString(int index) => UiValues.Text(At(index));

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
    readonly List<UiList> lists = [];
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

    // a list of single values, `data-for="name : names"` then {{name}}
    public void BindList<T>(string name, Func<IReadOnlyList<T>> items)
    {
        ArgumentNullException.ThrowIfNull(items);
        UiValues.EnsureSupported<T>("A data model list item");
        AddList(name, new UiList<T>(items, null), []);
    }

    // a list of records, `data-for="item : items"` then {{item.name}}, and {{items.size}} is its length. The getter
    // is how the list stays current, call Dirty with its name after it changes
    public void BindList<T>(string name, Func<IReadOnlyList<T>> items, Action<UiListFields<T>> fields)
    {
        ArgumentNullException.ThrowIfNull(items);
        ArgumentNullException.ThrowIfNull(fields);
        var declared = new UiListFields<T>();
        fields(declared);
        AddList(name, new UiList<T>(items, declared.Writers.ToArray()), declared.Names);
    }

    void AddList(string name, UiList list, List<string> fieldNames)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var fieldBytes = new byte[fieldNames.Count][];
        for (var i = 0; i < fieldBytes.Length; i++)
        {
            fieldBytes[i] = Encoding.UTF8.GetBytes(fieldNames[i]);
        }

        var views = new StringView[fieldBytes.Length];
        var pins = new GCHandle[fieldBytes.Length];
        try
        {
            for (var i = 0; i < fieldBytes.Length; i++)
            {
                pins[i] = GCHandle.Alloc(fieldBytes[i], GCHandleType.Pinned);
                views[i] = new StringView((byte*)pins[i].AddrOfPinnedObject(), fieldBytes[i].Length);
            }

            fixed (byte* data = nameBytes)
            fixed (StringView* fieldViews = views)
            {
                Ui.Check(
                    Runtime.Native.UiModelBindList(Live, new StringView(data, nameBytes.Length), lists.Count, fieldViews, views.Length),
                    $"Couldn't bind list {name}, is it already bound or the model in use?");
            }
        }
        finally
        {
            foreach (var pin in pins)
            {
                if (pin.IsAllocated)
                {
                    pin.Free();
                }
            }
        }

        lists.Add(list);
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

    internal int ListSize(int index) => (uint)index < (uint)lists.Count ? lists[index].Size() : 0;

    internal void ListGet(int index, int item, int field, Value* value)
    {
        if ((uint)index < (uint)lists.Count)
        {
            lists[index].Get(item, field, value, ref text);
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

internal static unsafe class UiValues
{
    public static void EnsureSupported<T>(string what)
    {
        if (typeof(T) != typeof(bool) && typeof(T) != typeof(int) && typeof(T) != typeof(long) &&
            typeof(T) != typeof(float) && typeof(T) != typeof(double) && typeof(T) != typeof(string))
        {
            throw new NotSupportedException($"{what} is a bool, int, long, float, double or string, not {typeof(T).Name}");
        }
    }

    // a string is copied into `text`, which the native side reads before it calls again
    public static void Write<T>(T current, Value* value, ref byte[] text)
    {
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

    public static long Integer(in Value value) => value.Kind is ValueKind.F64 or ValueKind.F32 ? (long)value.Number : value.Integer;

    public static double Number(in Value value) => value.Kind is ValueKind.F64 or ValueKind.F32 ? value.Number : value.Integer;

    public static string Text(in Value value) => value.Kind switch
    {
        ValueKind.String => Encoding.UTF8.GetString(value.Text.Data, (int)value.Text.Length),
        ValueKind.F64 or ValueKind.F32 => value.Number.ToString(System.Globalization.CultureInfo.InvariantCulture),
        _ => value.Integer.ToString(System.Globalization.CultureInfo.InvariantCulture),
    };
}

internal unsafe delegate void UiValueWriter<T>(T item, Value* value, ref byte[] text);

public sealed unsafe class UiListFields<T>
{
    internal readonly List<string> Names = [];
    internal readonly List<UiValueWriter<T>> Writers = [];

    internal UiListFields()
    {
    }

    // `Add("name", item => item.Name)`, `{{item.name}}` in the document
    public UiListFields<T> Add<TField>(string name, Func<T, TField> get)
    {
        ArgumentNullException.ThrowIfNull(get);
        UiValues.EnsureSupported<TField>($"A data model list field {name}");
        Names.Add(name);
        Writers.Add((T item, Value* value, ref byte[] text) => UiValues.Write(get(item), value, ref text));
        return this;
    }
}

internal abstract unsafe class UiList
{
    public abstract int Size();

    // `field` is -1 for the item itself
    public abstract void Get(int index, int field, Value* value, ref byte[] text);
}

internal sealed unsafe class UiList<T>(Func<IReadOnlyList<T>> items, UiValueWriter<T>[]? fields) : UiList
{
    public override int Size() => items()?.Count ?? 0;

    public override void Get(int index, int field, Value* value, ref byte[] text)
    {
        var current = items();
        if (current is null || (uint)index >= (uint)current.Count)
        {
            *value = default;
            return;
        }

        if (fields is null)
        {
            UiValues.Write(current[index], value, ref text);
        }
        else if ((uint)field < (uint)fields.Length)
        {
            fields[field](current[index], value, ref text);
        }
        else
        {
            *value = default;
        }
    }
}

internal abstract unsafe class UiBinding
{
    public abstract void Get(Value* value, ref byte[] text);
    public abstract void Set(Value* value);

    public static UiBinding Create<T>(Func<T> get, Action<T>? set)
    {
        UiValues.EnsureSupported<T>("A data model variable");
        return new UiBinding<T>(get, set);
    }
}

internal sealed unsafe class UiBinding<T>(Func<T> get, Action<T>? set) : UiBinding
{
    public override void Get(Value* value, ref byte[] text) => UiValues.Write(get(), value, ref text);

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
