using System;
using System.Numerics;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static class AudioSources
{
    extension(Entity entity)
    {
        // what the entity's AudioSourceComponent plays
        public AudioSource Audio => new(entity);
    }
}

public static unsafe class Audio
{
    // plays a loaded audio asset once, overlapping any other play of it. With a position it is spatialized there
    public static void PlayOneShot(AssetId sound, float volume = 1.0f, Vector3? position = null)
    {
        var at = position.GetValueOrDefault();
        var status = Runtime.Native.AudioOneShot(&sound, volume, position.HasValue ? &at : null);
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException($"Couldn't play {sound}, is it a loaded audio asset?");
        }
    }

    public static void PlayOneShot(AssetHandle sound, float volume = 1.0f, Vector3? position = null) =>
        PlayOneShot(sound.Id, volume, position);
}

// an entity's AudioSourceComponent sound. Its volume, pitch, looping and the rest are the component's fields, write
// them through entity.Get<AudioSourceComponent>() and the scene applies them each frame. A sound belongs to its
// asset, so two entities with the same one share its playback
public readonly unsafe struct AudioSource(Entity entity)
{
    public Entity Entity { get; } = entity;

    public bool IsPlaying => State.IsPlaying != 0;
    // seconds into the sound
    public float Cursor => State.Cursor;
    public float Length => State.Length;

    public void Play() => Command(AudioCommand.Play);
    public void Stop() => Command(AudioCommand.Stop);
    public void Pause() => Command(AudioCommand.Pause);
    public void Resume() => Command(AudioCommand.Resume);

    // the component takes its own reference on `sound` and gives back the one its previous sound held
    public void SetSound(AssetId sound)
    {
        if (Runtime.Native.AudioSetSound(Entity.World.Handle, Entity.Id, &sound) != OxStatus.Ok)
        {
            throw new InvalidOperationException($"{Entity} has no AudioSourceComponent, or {sound} isn't an audio asset");
        }
    }

    AudioState State
    {
        get
        {
            AudioState state;
            Check(Runtime.Native.AudioState(Entity.World.Handle, Entity.Id, &state));
            return state;
        }
    }

    void Command(AudioCommand command) => Check(Runtime.Native.AudioCommand(Entity.World.Handle, Entity.Id, command));

    void Check(OxStatus status)
    {
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException($"{Entity} has no AudioSourceComponent with a loaded sound");
        }
    }
}
