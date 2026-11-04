using System;
using System.IO;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public enum VfsDirectory
{
    // the program's own files
    App = VfsDir.App,
    // the game's content, what gameplay names, the same in the editor and in a shipped game
    Assets = VfsDir.Assets,
    Cooked = VfsDir.Cooked,
}

// the engine's virtual file system, for finding and reading files. Mounting stays with the engine and the editor, a
// script can't point a virtual directory somewhere else. A shipped game has its assets cooked into packs, so load
// those through Assets, and read files from here that the game ships as they are
public static unsafe class Vfs
{
    enum Operation
    {
        Resolve,
        ToPhysical,
        ToVirtual,
    }

    public static string Resolve(VfsDirectory directory, string relativePath) =>
        Call(Operation.Resolve, (VfsDir)directory, relativePath);

    // "assets_dir/Config/x.json" to where it is on disk, an absolute path passes through
    public static string ToPhysical(string path) => Call(Operation.ToPhysical, default, path);

    // the reverse, a path outside every mount comes back unchanged
    public static string ToVirtual(string path) => Call(Operation.ToVirtual, default, path);

    public static bool Exists(VfsDirectory directory, string relativePath) => File.Exists(Resolve(directory, relativePath));

    public static string ReadAllText(VfsDirectory directory, string relativePath) =>
        File.ReadAllText(Resolve(directory, relativePath));

    public static byte[] ReadAllBytes(VfsDirectory directory, string relativePath) =>
        File.ReadAllBytes(Resolve(directory, relativePath));

    // paths are short, one stack buffer nearly always does and a longer one is asked again at its real length
    static string Call(Operation operation, VfsDir directory, string path)
    {
        var bytes = Encoding.UTF8.GetBytes(path);
        fixed (byte* data = bytes)
        {
            var view = new StringView(data, bytes.Length);
            Span<byte> buffer = stackalloc byte[512];
            uint length;
            fixed (byte* output = buffer)
            {
                length = Write(operation, directory, view, output, (uint)buffer.Length);
            }

            if (length <= buffer.Length)
            {
                return Encoding.UTF8.GetString(buffer[..(int)length]);
            }

            var large = new byte[length];
            fixed (byte* output = large)
            {
                Write(operation, directory, view, output, length);
            }

            return Encoding.UTF8.GetString(large);
        }
    }

    static uint Write(Operation operation, VfsDir directory, StringView path, byte* output, uint capacity) => operation switch
    {
        Operation.Resolve => Runtime.Native.VfsResolve(directory, path, output, capacity),
        Operation.ToPhysical => Runtime.Native.VfsToPhysical(path, output, capacity),
        _ => Runtime.Native.VfsToVirtual(path, output, capacity),
    };
}
