using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static class CinematicPlayers
{
    extension(Entity entity)
    {
        // what the entity's CinematicPlayerComponent plays
        public CinematicPlayer Cinematic => new(entity);
    }
}

// runtime control over an entity's cinematic. All of it does nothing for an entity without a player or a world without a
// scene, and Time and Duration read 0 until the player has a cinematic
public readonly unsafe struct CinematicPlayer(Entity entity)
{
    public Entity Entity { get; } = entity;

    // seconds into the cinematic
    public float Time => State.Time;
    public float Duration => State.Duration;

    public void Play() => Command(CinematicCommand.Play, 0.0f);

    // also puts what the cinematic animated back, unless the component says otherwise
    public void Stop() => Command(CinematicCommand.Stop, 0.0f);

    // jumps to `time`, clamped to the cinematic's duration
    public void Seek(float time) => Command(CinematicCommand.Seek, time);

    CinematicState State
    {
        get
        {
            CinematicState state;
            Runtime.Native.SceneCinematicState(Entity.World.Handle, Entity.Id, &state);
            return state;
        }
    }

    void Command(CinematicCommand command, float value) =>
        Runtime.Native.SceneCinematicCommand(Entity.World.Handle, Entity.Id, command, value);
}
