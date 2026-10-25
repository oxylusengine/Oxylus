using System.Numerics;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static class ParticleEmitters
{
    extension(Entity entity)
    {
        // what the entity's ParticleSystemComponent emits
        public ParticleEmitter Particles => new(entity);
    }
}

// runtime control over an entity's emitter. Stopping only halts spawning, live particles run out their lifetime. All of
// it does nothing for an entity without an emitter or a world without a scene
public readonly unsafe struct ParticleEmitter(Entity entity)
{
    public Entity Entity { get; } = entity;

    public bool IsPlaying => Runtime.Native.SceneParticlesPlaying(Entity.World.Handle, Entity.Id);

    public void Play() => Command(ParticleCommand.Play, 0);
    public void Stop() => Command(ParticleCommand.Stop, 0);

    // rewinds the emitter's time, so Once nodes and the duration window fire again. Live particles are untouched
    public void Restart() => Command(ParticleCommand.Restart, 0);

    // queues an extra spawn for the next frame, whether or not the emitter is playing
    public void EmitBurst(uint count) => Command(ParticleCommand.EmitBurst, count);

    public void SetParameter(uint index, Vector4 value) =>
        Runtime.Native.SceneParticlesSetParameter(Entity.World.Handle, Entity.Id, index, &value);

    // false when the emitter's asset has no parameter by that name
    public bool SetParameter(string name, Vector4 value)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* data = bytes)
        {
            return Runtime.Native.SceneParticlesSetParameterNamed(
                Entity.World.Handle, Entity.Id, new StringView(data, bytes.Length), &value);
        }
    }

    void Command(ParticleCommand command, uint value) =>
        Runtime.Native.SceneParticlesCommand(Entity.World.Handle, Entity.Id, command, value);
}
