using System;
using System.Buffers;
using System.Globalization;
using System.Runtime.CompilerServices;

namespace Oxylus;

// what `Log.Info($"hit {entity} for {damage}")` builds instead of a string: numbers and other ISpanFormattable values
// format straight into a pooled buffer, so a log line costs no allocation. Always the invariant culture, a log reads
// the same on every machine and a shipped game has no other culture anyway
[InterpolatedStringHandler]
public ref struct LogMessage
{
    char[] buffer;
    int position;

    public LogMessage(int literalLength, int formattedCount)
    {
        buffer = ArrayPool<char>.Shared.Rent(Math.Max(literalLength + formattedCount * 11, 64));
        position = 0;
    }

    internal readonly ReadOnlySpan<char> Text => buffer.AsSpan(0, position);

    public void AppendLiteral(string value) => AppendSpan(value);

    public void AppendFormatted(string? value) => AppendSpan(value);

    public void AppendFormatted(ReadOnlySpan<char> value) => AppendSpan(value);

    public void AppendFormatted<T>(T value) => AppendFormatted(value, null);

    // straight to optimized code, the tier-0 JIT boxes the value where the optimized one doesn't
    [MethodImpl(MethodImplOptions.AggressiveOptimization)]
    public void AppendFormatted<T>(T value, string? format)
    {
        // written as a type test and a cast, the shape the JIT and NativeAOT turn into a direct call without boxing a
        // value type, as DefaultInterpolatedStringHandler does
        if (value is ISpanFormattable)
        {
            int written;
            while (!((ISpanFormattable)value).TryFormat(buffer.AsSpan(position), out written, format, CultureInfo.InvariantCulture))
            {
                Grow(buffer.Length);
            }

            position += written;
            return;
        }

        AppendSpan(value is IFormattable formattable ? formattable.ToString(format, CultureInfo.InvariantCulture) : value?.ToString());
    }

    public void AppendFormatted<T>(T value, int alignment) => AppendFormatted(value, alignment, null);

    public void AppendFormatted<T>(T value, int alignment, string? format)
    {
        var start = position;
        AppendFormatted(value, format);
        var width = position - start;
        var padding = Math.Abs(alignment) - width;
        if (padding <= 0)
        {
            return;
        }

        EnsureSpace(padding);
        if (alignment > 0)
        {
            buffer.AsSpan(start, width).CopyTo(buffer.AsSpan(start + padding));
            buffer.AsSpan(start, padding).Fill(' ');
        }
        else
        {
            buffer.AsSpan(position, padding).Fill(' ');
        }

        position += padding;
    }

    internal void Dispose()
    {
        var rented = buffer;
        this = default;
        if (rented is not null)
        {
            ArrayPool<char>.Shared.Return(rented);
        }
    }

    void AppendSpan(ReadOnlySpan<char> value)
    {
        EnsureSpace(value.Length);
        value.CopyTo(buffer.AsSpan(position));
        position += value.Length;
    }

    void EnsureSpace(int length)
    {
        if (position + length > buffer.Length)
        {
            Grow(position + length - buffer.Length);
        }
    }

    void Grow(int atLeast)
    {
        var grown = ArrayPool<char>.Shared.Rent(Math.Max(buffer.Length * 2, buffer.Length + atLeast));
        buffer.AsSpan(0, position).CopyTo(grown);
        ArrayPool<char>.Shared.Return(buffer);
        buffer = grown;
    }
}
