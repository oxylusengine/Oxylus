using System;
using System.Numerics;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

public static unsafe class Camera
{
    // the world space ray through a point of the screen, from the entity's CameraComponent as the scene last updated it.
    // A camera that hasn't been updated yet gives a zero ray
    public static Ray ScreenRay(Entity camera, Vector2 screenPosition, Vector2 screenSize)
    {
        RayData ray;
        var status = Runtime.Native.PhysicsScreenRay(camera.World.Handle, camera.Id, &screenPosition, &screenSize, &ray);
        if (status != OxStatus.Ok)
        {
            throw new InvalidOperationException($"{camera} has no CameraComponent");
        }

        return new Ray(ray.Origin, ray.Direction);
    }
}
