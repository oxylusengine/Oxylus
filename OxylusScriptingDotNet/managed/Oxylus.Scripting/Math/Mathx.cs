using System;
using System.Numerics;

namespace Oxylus;

// what the glm helpers are not in System.Numerics or the float statics. The rest map one to one: angle_axis is
// Quaternion.CreateFromAxisAngle, look_at Matrix4x4.CreateLookAt, translate Matrix4x4.CreateTranslation, mix and
// slerp Lerp and Quaternion.Slerp, atan2 MathF.Atan2, radians and degrees float.DegreesToRadians and
// float.RadiansToDegrees, and clamp, cross, distance, dot, inverse, length and normalize their System.Numerics twins
public static class Mathx
{
    // glm::quatLookAt, the rotation that turns -Z to `direction`, which has to be normalized. `up` is never parallel
    // to it
    public static Quaternion QuatLookAt(Vector3 direction, Vector3 up)
    {
        var back = -direction;
        var right = Vector3.Cross(up, back);
        right *= 1.0f / MathF.Sqrt(MathF.Max(0.00001f, Vector3.Dot(right, right)));
        var newUp = Vector3.Cross(back, right);

        // glm's columns are System.Numerics' rows
        var basis = new Matrix4x4(
            right.X, right.Y, right.Z, 0.0f,
            newUp.X, newUp.Y, newUp.Z, 0.0f,
            back.X, back.Y, back.Z, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
        return Quaternion.CreateFromRotationMatrix(basis);
    }

    // glm::round, halves away from zero rather than to even
    public static float Round(float value) => MathF.Round(value, MidpointRounding.AwayFromZero);

    public static Vector2 Floor(Vector2 value) => new(MathF.Floor(value.X), MathF.Floor(value.Y));
    public static Vector3 Floor(Vector3 value) => new(MathF.Floor(value.X), MathF.Floor(value.Y), MathF.Floor(value.Z));

    public static Vector4 Floor(Vector4 value) =>
        new(MathF.Floor(value.X), MathF.Floor(value.Y), MathF.Floor(value.Z), MathF.Floor(value.W));

    public static Vector2 Ceil(Vector2 value) => new(MathF.Ceiling(value.X), MathF.Ceiling(value.Y));
    public static Vector3 Ceil(Vector3 value) => new(MathF.Ceiling(value.X), MathF.Ceiling(value.Y), MathF.Ceiling(value.Z));

    public static Vector4 Ceil(Vector4 value) =>
        new(MathF.Ceiling(value.X), MathF.Ceiling(value.Y), MathF.Ceiling(value.Z), MathF.Ceiling(value.W));

    public static Vector2 Round(Vector2 value) => new(Round(value.X), Round(value.Y));
    public static Vector3 Round(Vector3 value) => new(Round(value.X), Round(value.Y), Round(value.Z));
    public static Vector4 Round(Vector4 value) => new(Round(value.X), Round(value.Y), Round(value.Z), Round(value.W));
}
