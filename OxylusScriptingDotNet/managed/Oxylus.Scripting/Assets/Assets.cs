using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// Asset/AssetFile.hpp's AssetType
public enum AssetType : uint
{
    None = 0,
    Shader,
    Model,
    Texture,
    Material,
    Font,
    Scene,
    Audio,
    Script,
    Terrain,
    ParticleSystem,
    Skeleton,
    Animation,
    Cinematic,
    UI,
}

// an asset's UUID. Naming one holds nothing, an AssetHandle is what keeps the asset loaded
[StructLayout(LayoutKind.Sequential)]
public readonly struct AssetId : IEquatable<AssetId>
{
    readonly ulong low;
    readonly ulong high;

    public bool IsValid => low != 0 && high != 0;

    public bool Equals(AssetId other) => low == other.low && high == other.high;
    public override bool Equals(object? obj) => obj is AssetId other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(low, high);

    // as UUID::str writes it, so it reads the same as a sidecar or scene file
    public override string ToString() =>
        $"{(uint)(low >> 32):x8}-{(low >> 16) & 0xffff:x4}-{low & 0xffff:x4}-{high >> 48:x4}-{high & 0xffffffffffff:x12}";

    public static bool operator ==(AssetId left, AssetId right) => left.Equals(right);
    public static bool operator !=(AssetId left, AssetId right) => !left.Equals(right);
}

// one reference on an asset, given back by Dispose. Handles left undisposed when the scripts unload are released then,
// with a warning, so a reload never strands a reference. Dispose on the main thread, as everything here runs
public sealed unsafe class AssetHandle : IDisposable
{
    internal static readonly List<AssetHandle> Live = [];

    AssetId id;

    internal AssetHandle(AssetId id)
    {
        this.id = id;
        Live.Add(this);
    }

    public AssetId Id => id;
    public bool IsDisposed => !id.IsValid;

    public AssetType Type => Assets.Info(id).Type;
    public bool IsLoaded => Assets.Info(id).IsLoaded;

    public void Dispose()
    {
        if (!id.IsValid)
        {
            return;
        }

        var released = id;
        id = default;
        Live.Remove(this);
        Runtime.Native.AssetRelease(&released);
    }

    internal static int ReleaseAll()
    {
        var count = Live.Count;
        for (var i = Live.Count - 1; i >= 0; i--)
        {
            Live[i].Dispose();
        }

        return count;
    }
}

public readonly record struct AssetDetails(AssetType Type, bool IsLoaded, ulong RefCount);

public static unsafe class Assets
{
    // `path` is relative to the game's assets, "Audio/door.wav", what the asset was imported from. default when
    // nothing was
    public static AssetId Find(string path)
    {
        var bytes = Encoding.UTF8.GetBytes(path);
        AssetId id;
        fixed (byte* data = bytes)
        {
            return Runtime.Native.AssetFind(new StringView(data, bytes.Length), &id) ? id : default;
        }
    }

    // loads the asset if it isn't, and holds it until the handle is disposed
    public static AssetHandle Acquire(AssetId id)
    {
        if (!id.IsValid || !Runtime.Native.AssetAcquire(&id))
        {
            throw new InvalidOperationException($"Couldn't load asset {id}");
        }

        return new AssetHandle(id);
    }

    public static AssetHandle Acquire(string path)
    {
        var id = Find(path);
        if (!id.IsValid)
        {
            throw new InvalidOperationException($"No asset was imported from {path}");
        }

        return Acquire(id);
    }

    public static AssetDetails Info(AssetId id)
    {
        AssetInfo info;
        return Runtime.Native.AssetInfo(&id, &info)
            ? new AssetDetails((AssetType)info.Type, info.IsLoaded != 0, info.RefCount)
            : default;
    }

    // a loaded model's material assets. Fills `materials` and returns how many the model has, which can be more than
    // fit, and 0 for an id that isn't a loaded model. The materials are held by the model, acquire one to keep it
    // past the model
    public static int GetMaterials(AssetId model, Span<AssetId> materials)
    {
        int count;
        fixed (AssetId* data = materials)
        {
            return Runtime.Native.AssetMaterials(&model, data, materials.Length, &count) == OxStatus.Ok ? count : 0;
        }
    }

    public static AssetId[] GetMaterials(AssetId model)
    {
        var count = GetMaterials(model, []);
        if (count == 0)
        {
            return [];
        }

        var materials = new AssetId[count];
        GetMaterials(model, materials);
        return materials;
    }

    // false when `material` isn't a loaded material asset
    public static bool GetMaterial(AssetId material, out MaterialData data)
    {
        data = default;
        fixed (MaterialData* result = &data)
        {
            return Runtime.Native.AssetMaterialGet(&material, result) == OxStatus.Ok;
        }
    }

    // writes the parameters and has the renderer pick them up. A texture the material stops naming is released and
    // one it starts naming is loaded and held, exactly as many times as the material itself is
    public static void SetMaterial(AssetId material, in MaterialData data)
    {
        fixed (MaterialData* source = &data)
        {
            var status = Runtime.Native.AssetMaterialSet(&material, source);
            if (status != OxStatus.Ok)
            {
                throw new InvalidOperationException(status == OxStatus.NotFound
                    ? $"{material} isn't a loaded material"
                    : $"Couldn't set {material}, is a texture slot not a texture asset or an enum out of range?");
            }
        }
    }

    internal static void ReleaseScripts()
    {
        var count = AssetHandle.ReleaseAll();
        if (count > 0)
        {
            Log.Warn($"{count} asset handles were still held when the scripts unloaded, they are released now");
        }
    }
}
