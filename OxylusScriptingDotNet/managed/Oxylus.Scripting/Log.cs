using System;
using System.Buffers;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static unsafe class Log
{
    const int StackBufferSize = 1024;

    public static void Trace(string message) => Write(LogLevel.Trace, message);
    public static void Debug(string message) => Write(LogLevel.Debug, message);
    public static void Info(string message) => Write(LogLevel.Info, message);
    public static void Warn(string message) => Write(LogLevel.Warn, message);
    public static void Error(string message) => Write(LogLevel.Error, message);

    // an interpolated string goes here rather than to the string overloads, and allocates nothing
    public static void Trace(ref LogMessage message) => Write(LogLevel.Trace, ref message);
    public static void Debug(ref LogMessage message) => Write(LogLevel.Debug, ref message);
    public static void Info(ref LogMessage message) => Write(LogLevel.Info, ref message);
    public static void Warn(ref LogMessage message) => Write(LogLevel.Warn, ref message);
    public static void Error(ref LogMessage message) => Write(LogLevel.Error, ref message);

    // `Log.Info("hello"u8)` skips the encode
    public static void Trace(ReadOnlySpan<byte> utf8) => Write(LogLevel.Trace, utf8);
    public static void Debug(ReadOnlySpan<byte> utf8) => Write(LogLevel.Debug, utf8);
    public static void Info(ReadOnlySpan<byte> utf8) => Write(LogLevel.Info, utf8);
    public static void Warn(ReadOnlySpan<byte> utf8) => Write(LogLevel.Warn, utf8);
    public static void Error(ReadOnlySpan<byte> utf8) => Write(LogLevel.Error, utf8);

    static void Write(LogLevel level, ref LogMessage message)
    {
        Write(level, message.Text);
        message.Dispose();
    }

    static void Write(LogLevel level, ReadOnlySpan<char> message)
    {
        var maxLength = Encoding.UTF8.GetMaxByteCount(message.Length);
        byte[]? rented = null;
        var buffer = maxLength <= StackBufferSize
            ? stackalloc byte[StackBufferSize]
            : (rented = ArrayPool<byte>.Shared.Rent(maxLength));

        var length = Encoding.UTF8.GetBytes(message, buffer);
        Write(level, buffer[..length]);

        if (rented is not null)
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }

    static void Write(LogLevel level, ReadOnlySpan<byte> utf8)
    {
        var log = Runtime.Native.Log;
        if (log is null)
        {
            return;
        }

        fixed (byte* message = utf8)
        {
            log(level, message, (nuint)utf8.Length);
        }
    }
}
