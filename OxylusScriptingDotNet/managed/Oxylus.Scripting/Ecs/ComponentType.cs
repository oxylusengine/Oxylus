using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

using Oxylus.Interop;

namespace Oxylus;

// a component type's slot in each world's id table, assigned once when the scripts load
public static class ComponentType<T> where T : unmanaged
{
    public static int Index = -1;
}

public static class ComponentType
{
    public static uint AlignmentOf<T>() where T : unmanaged => (uint)Unsafe.SizeOf<AlignmentProbe<T>>() - (uint)Unsafe.SizeOf<T>();

    // `value` as bytes, or null when they are all zero, which is what flecs gives a new component anyway
    public static byte[]? DefaultsOf<T>(in T value) where T : unmanaged
    {
        var bytes = MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(in value, 1));
        return bytes.IndexOfAnyExcept((byte)0) < 0 ? null : bytes.ToArray();
    }

    [StructLayout(LayoutKind.Sequential)]
    struct AlignmentProbe<T> where T : unmanaged
    {
#pragma warning disable CS0649
        public byte Padding;
        public T Value;
#pragma warning restore CS0649
    }
}

public sealed record MemberDescriptor(byte[] Name, uint Offset, uint Size, ComponentFieldType Type = ComponentFieldType.F32);

// `IsScriptDefined` components are created in each world from their members, the rest already exist natively.
// `Defaults` is what a new instance of a script-defined component starts as, null for all zero
public sealed record ComponentDescriptor(
    byte[] Path,
    uint Size,
    uint Alignment,
    MemberDescriptor[] Members,
    bool IsScriptDefined = false,
    byte[]? Defaults = null);

public readonly record struct TermDescriptor(int ComponentIndex, TermAccess Access);

public sealed unsafe class SystemDescriptor(
    byte[] name,
    Phase phase,
    TermDescriptor[] terms,
    delegate* unmanaged<TableView*, void> run)
{
    public byte[] Name { get; } = name;
    public Phase Phase { get; } = phase;
    public TermDescriptor[] Terms { get; } = terms;
    public delegate* unmanaged<TableView*, void> Run { get; } = run;
}
