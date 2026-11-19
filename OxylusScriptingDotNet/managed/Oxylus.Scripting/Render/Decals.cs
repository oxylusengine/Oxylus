using System;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static class Decals
{
    extension(Entity entity)
    {
        // what the entity's DecalComponent projects
        public Decal Decal => new(entity);
    }
}

// an entity's DecalComponent material. Opacity, sort order, the fades, the layer mask and the affect flags are the
// component's fields, write them through entity.Get<DecalComponent>() and the renderer picks them up next frame. Set
// the material only through here, writing the field directly skips the reference the component owns. Add the
// component with Add<DecalComponent>(), a C# `new DecalComponent()` is all zeroes and so fully transparent
public readonly unsafe struct Decal(Entity entity)
{
    public Entity Entity { get; } = entity;

    public AssetId Material => Entity.Get<DecalComponent>().Material;

    // the component takes its own reference on `material` and gives back the one its previous material held. default
    // clears it
    public void SetMaterial(AssetId material)
    {
        var status = Runtime.Native.DecalSetMaterial(Entity.World.Handle, Entity.Id, &material);
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException(status == OxStatus.InvalidArgument
                ? $"{material} isn't a material asset"
                : $"{Entity} has no DecalComponent, or {material} isn't a known asset");
        }
    }
}
