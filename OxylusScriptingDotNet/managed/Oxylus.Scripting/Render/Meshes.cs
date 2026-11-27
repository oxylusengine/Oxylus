using System;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static class Meshes
{
    extension(Entity entity)
    {
        // what the entity's MeshComponent draws with
        public Mesh Mesh => new(entity);
    }
}

// an entity's MeshComponent material override. The rest of the component is plain fields, write them through
// entity.Get<MeshComponent>(). Set the material only through here, writing the field directly skips the reference
// the component owns
public readonly unsafe struct Mesh(Entity entity)
{
    public Entity Entity { get; } = entity;

    // default while the mesh draws with its model's own material
    public AssetId Material => Entity.Get<MeshComponent>().MaterialUuid;

    // the component takes its own reference on `material` and gives back the one its previous material held. default
    // goes back to the model's own material
    public void SetMaterial(AssetId material)
    {
        var status = Runtime.Native.MeshSetMaterial(Entity.World.Handle, Entity.Id, &material);
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException(status == OxStatus.InvalidArgument
                ? $"{material} isn't a material asset"
                : $"{Entity} has no MeshComponent, or {material} isn't a known asset");
        }
    }
}
