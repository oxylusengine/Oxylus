using System;
using System.Globalization;

using Oxylus;

namespace SmokeGame;

[Component]
public struct Spin
{
    public float Speed;
    public float Angle;
}

public sealed class LevelLogic : SceneBehaviour
{
    int updates;

    public override void OnAdd() => Log.Info("LevelLogic.OnAdd");
    public override void OnRemove() => Log.Info("LevelLogic.OnRemove");
    public override void OnStart() => Log.Info("LevelLogic.OnStart");
    public override void OnStop() => Log.Info($"LevelLogic.OnStop updates={updates}");
    public override void OnUpdate(float deltaTime) => updates++;

    public override void OnFixedUpdate(float deltaTime) =>
        Log.Info($"LevelLogic.OnFixedUpdate {deltaTime.ToString("F4", CultureInfo.InvariantCulture)}");

    public override void OnContact(in Contact contact)
    {
        if (contact.Event == ContactEvent.Added)
        {
            Log.Info($"LevelLogic.OnContact {contact.Self.Name} {contact.Other.Name}");
        }
    }

    // only runs in scenes that have a LevelLogic
    [System(Phase.OnUpdate)]
    public static void Turn(ref Spin spin, float deltaTime) => spin.Angle += spin.Speed * deltaTime;
}

public sealed class Spinner : EntityBehaviour
{
    public override void OnCreate() => Log.Info($"Spinner.OnCreate {Entity.Name}");
    public override void OnStart() => Log.Info($"Spinner.OnStart {Entity.Name}");
    public override void OnDestroy() => Log.Info($"Spinner.OnDestroy {Entity.Name}");

    public override void OnUpdate(float deltaTime)
    {
        Entity.Get<TransformComponent>().Position.X += 1.0f;
        Entity.Modified<TransformComponent>();
    }
}

public sealed class Faulty : EntityBehaviour
{
    public override void OnUpdate(float deltaTime) => throw new InvalidOperationException("faulty on purpose");
}

// gives the entity named "Late" a Spinner from inside an update, through the Entity API
public sealed class Spawner : EntityBehaviour
{
    public override void OnUpdate(float deltaTime)
    {
        var late = World.Lookup("Late");
        if (late.IsAlive && late.GetBehaviour<Spinner>() is null)
        {
            late.AddBehaviour<Spinner>();
        }
    }
}

public sealed class Bumper : EntityBehaviour
{
    int mainThread;

    public override void OnCreate() => mainThread = Environment.CurrentManagedThreadId;

    public override void OnContact(in Contact contact)
    {
        if (contact.Event != ContactEvent.Added)
        {
            return;
        }

        var normal = contact.Normal.Y.ToString("F1", CultureInfo.InvariantCulture);
        var isMainThread = Environment.CurrentManagedThreadId == mainThread;
        Log.Info($"Bumper.OnContact {contact.Self.Name} {contact.Other.Name} {normal} main={isMainThread}");
    }
}

// the cheapest behaviour there is, what a frame of them costs is the dispatch
public sealed class Ticker : EntityBehaviour
{
    public int Ticks;

    public override void OnUpdate(float deltaTime) => Ticks++;
}
