using System;
using System.Runtime.InteropServices;

using Oxylus.Interop;

namespace Oxylus.Hosting;

// one native block for the strings and arrays a single native call reads, freed right after it
internal unsafe ref struct Scratch
{
    readonly byte* start;
    readonly nuint capacity;
    nuint used;

    public Scratch(int capacity)
    {
        this.capacity = (nuint)capacity;
        start = (byte*)NativeMemory.AlignedAlloc(this.capacity, 16);
    }

    public T* Alloc<T>(int count) where T : unmanaged
    {
        used = (used + 15) & ~(nuint)15;
        var size = (nuint)(count * sizeof(T));
        if (used + size > capacity)
        {
            throw new InvalidOperationException("Scratch is too small");
        }

        var result = (T*)(start + used);
        used += size;
        return result;
    }

    public StringView String(byte[] utf8)
    {
        var data = Alloc<byte>(utf8.Length);
        utf8.CopyTo(new Span<byte>(data, utf8.Length));
        return new StringView(data, utf8.Length);
    }

    // a bound on what String() and Alloc() of these will take, padding included
    public static int SizeFor(int bytes, int allocations) => bytes + allocations * 16;

    public readonly void Dispose() => NativeMemory.AlignedFree(start);
}
