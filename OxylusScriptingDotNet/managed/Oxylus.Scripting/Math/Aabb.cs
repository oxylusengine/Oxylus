using System;
using System.Numerics;

namespace Oxylus;

// Render/BoundingVolume.hpp's Intersection
public enum Intersection
{
    Outside = 0,
    Intersects,
    Inside,
}

// an axis aligned box, Render/BoundingVolume.hpp's AABB
public readonly struct Aabb(Vector3 min, Vector3 max) : IEquatable<Aabb>
{
    public Vector3 Min { get; } = min;
    public Vector3 Max { get; } = max;

    public Vector3 Center => (Max + Min) * 0.5f;
    public Vector3 Size => Max - Min;
    public Vector3 Extents => Size * 0.5f;

    // `size` is the whole width, height and depth
    public static Aabb FromBounds(Vector3 center, Vector3 size) => new(center - size * 0.5f, center + size * 0.5f);

    public Aabb Translate(Vector3 translation) => new(Min + translation, Max + translation);

    // about the origin, not the centre
    public Aabb Scale(Vector3 scale) => new(Min * scale, Max * scale);

    // the box around the transformed box, rotation grows it. `matrix` is read the way a glm::mat4 with the same memory is
    public Aabb Transform(in Matrix4x4 matrix)
    {
        var center = Vector3.Transform(Center, matrix);
        var edge = Extents;
        var newEdge = new Vector3(
            MathF.Abs(matrix.M11) * edge.X + MathF.Abs(matrix.M21) * edge.Y + MathF.Abs(matrix.M31) * edge.Z,
            MathF.Abs(matrix.M12) * edge.X + MathF.Abs(matrix.M22) * edge.Y + MathF.Abs(matrix.M32) * edge.Z,
            MathF.Abs(matrix.M13) * edge.X + MathF.Abs(matrix.M23) * edge.Y + MathF.Abs(matrix.M33) * edge.Z);
        return new Aabb(center - newEdge, center + newEdge);
    }

    // the smallest box around both
    public Aabb Merge(Aabb other) => new(Vector3.Min(Min, other.Min), Vector3.Max(Max, other.Max));

    // a point on the surface is inside
    public bool Contains(Vector3 point) =>
        point.X >= Min.X && point.X <= Max.X &&
        point.Y >= Min.Y && point.Y <= Max.Y &&
        point.Z >= Min.Z && point.Z <= Max.Z;

    // boxes that only touch intersect
    public bool Intersects(Aabb other) =>
        other.Max.X >= Min.X && other.Min.X <= Max.X &&
        other.Max.Y >= Min.Y && other.Min.Y <= Max.Y &&
        other.Max.Z >= Min.Z && other.Min.Z <= Max.Z;

    // where `other` is relative to this box: apart, overlapping, or entirely within
    public Intersection Classify(Aabb other)
    {
        if (!Intersects(other))
        {
            return Intersection.Outside;
        }

        var isInside = other.Min.X >= Min.X && other.Max.X <= Max.X &&
                       other.Min.Y >= Min.Y && other.Max.Y <= Max.Y &&
                       other.Min.Z >= Min.Z && other.Max.Z <= Max.Z;
        return isInside ? Intersection.Inside : Intersection.Intersects;
    }

    public bool Equals(Aabb other) => Min == other.Min && Max == other.Max;
    public override bool Equals(object? obj) => obj is Aabb other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Min, Max);
    public override string ToString() => $"Aabb({Min}, {Max})";

    public static bool operator ==(Aabb left, Aabb right) => left.Equals(right);
    public static bool operator !=(Aabb left, Aabb right) => !left.Equals(right);
}
