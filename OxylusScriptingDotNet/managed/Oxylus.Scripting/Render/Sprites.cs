using System;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static class Sprites
{
    extension(Entity entity)
    {
        // what the entity's SpriteComponent draws with
        public Sprite Sprite => new(entity);
    }
}

// an entity's SpriteComponent material. The rest of the component is plain fields, write them through
// entity.Get<SpriteComponent>(). Set the material only through here, writing the field directly skips the reference
// the component owns
public readonly unsafe struct Sprite(Entity entity)
{
    public Entity Entity { get; } = entity;

    public AssetId Material => Entity.Get<SpriteComponent>().Material;

    // the component takes its own reference on `material` and gives back the one its previous material held. default
    // clears it
    public void SetMaterial(AssetId material)
    {
        var status = Runtime.Native.SpriteSetMaterial(Entity.World.Handle, Entity.Id, &material);
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException(status == OxStatus.InvalidArgument
                ? $"{material} isn't a material asset"
                : $"{Entity} has no SpriteComponent, or {material} isn't a known asset");
        }
    }
}
