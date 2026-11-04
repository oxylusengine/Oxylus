using System.Numerics;

namespace Oxylus;

// Position.Y is ignored, the track follows the ground
public readonly record struct TerrainWaypoint(Vector3 Position, float Width, float Shoulder);
