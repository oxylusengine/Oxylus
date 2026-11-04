using System.Numerics;

using Oxylus;

namespace SmokeGame;

[Component]
public struct Velocity
{
    public Vector3 Value;
}

[Component]
public struct Countdown
{
    public float Seconds;
    public int Ticks;
}

[Component]
public struct Expired;

// counts the movers each frame through a query, so the query API runs inside a real world
[Component]
public struct Census
{
    public int Movers;
}

public static class Movement
{
    [System(Phase.OnUpdate)]
    public static void Move(ref TransformComponent transform, in Velocity velocity, float deltaTime) =>
        transform.Position += velocity.Value * deltaTime;

    // runs after Move, and goes through the Entity API instead of its parameters
    [System(Phase.PostUpdate)]
    public static void Tick(Entity entity, ref Countdown countdown, float deltaTime)
    {
        countdown.Ticks++;
        countdown.Seconds -= deltaTime;
        if (countdown.Seconds > 0.0f || entity.Has<Expired>())
        {
            return;
        }

        entity.Add<Expired>();
        entity.Get<TransformComponent>().Scale = Vector3.Zero;
        entity.Modified<TransformComponent>();
        entity.Set(new Velocity { Value = Vector3.Zero });
    }

    [System(Phase.PostUpdate)]
    public static void Count(Entity entity, ref Census census)
    {
        using var movers = entity.World.Query<TransformComponent, Velocity>();
        census.Movers = 0;
        foreach (var table in movers)
        {
            census.Movers += table.Count;
        }
    }
}
