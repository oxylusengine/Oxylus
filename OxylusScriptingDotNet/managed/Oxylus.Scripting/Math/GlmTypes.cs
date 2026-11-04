using System.Numerics;
using System.Runtime.InteropServices;

namespace Oxylus;

// glm types System.Numerics has no match for, laid out exactly like glm's

[StructLayout(LayoutKind.Sequential)]
public struct Int2(int x, int y)
{
    public int X = x;
    public int Y = y;
}

[StructLayout(LayoutKind.Sequential)]
public struct UInt3(uint x, uint y, uint z)
{
    public uint X = x;
    public uint Y = y;
    public uint Z = z;
}

// column major like glm::mat3
[StructLayout(LayoutKind.Sequential)]
public struct Matrix3x3(Vector3 column0, Vector3 column1, Vector3 column2)
{
    public Vector3 Column0 = column0;
    public Vector3 Column1 = column1;
    public Vector3 Column2 = column2;
}
