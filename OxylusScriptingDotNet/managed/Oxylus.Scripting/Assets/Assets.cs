using System;
using System.Collections.Generic;
using System.Numerics;
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

// one mesh of a model made with Assets.CreateModel, a triangle list. Normals are empty or one per position, and the
// spans are only read during the call
public readonly ref struct MeshSource
{
    public ReadOnlySpan<Vector3> Positions { get; init; }
    public ReadOnlySpan<Vector3> Normals { get; init; }
    public ReadOnlySpan<uint> Indices { get; init; }
    public AssetId Material { get; init; }
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

    // a material asset of its own, loaded and held for as long as the game runs
    public static AssetId CreateMaterial(in MaterialData data)
    {
        AssetId material;
        fixed (MaterialData* source = &data)
        {
            var status = Runtime.Native.AssetCreateMaterial(source, &material);
            if (status != OxStatus.Ok)
            {
                throw new InvalidOperationException(status == OxStatus.NotFound
                    ? "Couldn't create a material"
                    : "Couldn't create a material, is a texture slot not a texture asset or an enum out of range?");
            }
        }

        return material;
    }

    // a model of one or two meshes, ready for World.SpawnModel when this returns. A MeshColliderComponent with Convex
    // set on the entity SpawnModel returns builds one hull from the positions of both meshes. A mesh without indices
    // is left out. Hand it back with DestroyModel, it goes once the entities spawned from it are gone too
    public static AssetId CreateModel(MeshSource first, MeshSource second = default)
    {
        Span<MeshSourceData> meshes = stackalloc MeshSourceData[2];
        AssetId model;
        OxStatus status;
        fixed (Vector3* firstPositions = first.Positions)
        fixed (Vector3* firstNormals = first.Normals)
        fixed (uint* firstIndices = first.Indices)
        fixed (Vector3* secondPositions = second.Positions)
        fixed (Vector3* secondNormals = second.Normals)
        fixed (uint* secondIndices = second.Indices)
        fixed (MeshSourceData* data = meshes)
        {
            meshes[0] = Describe(first, firstPositions, firstNormals, firstIndices);
            meshes[1] = Describe(second, secondPositions, secondNormals, secondIndices);
            status = Runtime.Native.AssetCreateModel(data, 2, &model);
        }

        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException(status == OxStatus.NotFound
                ? "Couldn't create a model, it takes the renderer"
                : "Couldn't create a model, are the indices whole triangles within the positions, are the normals one per "
                  + "position, and is the material a loaded material?");
        }

        return model;
    }

    public static void DestroyModel(AssetId model)
    {
        if (Runtime.Native.AssetDestroyModel(&model) != OxStatus.Ok)
        {
            throw new InvalidOperationException($"{model} isn't a model made by CreateModel, or it was destroyed already");
        }
    }

    static MeshSourceData Describe(in MeshSource source, Vector3* positions, Vector3* normals, uint* indices)
    {
        if (source.Normals.Length != 0 && source.Normals.Length != source.Positions.Length)
        {
            throw new ArgumentException("a mesh needs no normals or one per position");
        }

        return new MeshSourceData
        {
            Positions = positions,
            Normals = source.Normals.Length != 0 ? normals : null,
            Indices = indices,
            VertexCount = (uint)source.Positions.Length,
            IndexCount = (uint)source.Indices.Length,
            Material = source.Material,
        };
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
