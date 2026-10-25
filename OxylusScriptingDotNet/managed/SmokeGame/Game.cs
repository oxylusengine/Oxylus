using System;

using Oxylus;

namespace SmokeGame;

// what the SmokeGame executable's start scene runs, the test scripts in the other files never reach a shipped game

[Component]
public struct Frames
{
    public int Count;
}

// counts frames on an entity named "Stats", which `SmokeGame --smoke-test` reads back to prove the scripts ran
public sealed class GameLoop : SceneScript
{
    Entity stats;

    public override void OnStart()
    {
        stats = World.CreateEntity("Stats").Set(new Frames());
        Log.Info("SmokeGame started");
    }

    public override void OnUpdate(float deltaTime) => stats.Get<Frames>().Count++;

    public override void OnStop() => Log.Info($"SmokeGame stopped after {stats.Get<Frames>().Count} frames");
}

public sealed class CameraBob : Behaviour
{
    float time;
    float baseY;

    public override void OnStart() => baseY = Entity.Get<TransformComponent>().Position.Y;

    public override void OnUpdate(float deltaTime)
    {
        time += deltaTime;
        Entity.Get<TransformComponent>().Position.Y = baseY + MathF.Sin(time) * 0.25f;
        Entity.Modified<TransformComponent>();
    }
}
