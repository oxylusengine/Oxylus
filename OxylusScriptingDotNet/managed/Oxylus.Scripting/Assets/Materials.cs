using System.Numerics;
using System.Runtime.InteropServices;

namespace Oxylus;

// Asset/Material.hpp's AlphaMode
public enum AlphaMode : uint
{
    Opaque = 0,
    Mask,
    Blend,
}

// Asset/Material.hpp's SamplingMode
public enum SamplingMode : uint
{
    LinearRepeated = 0,
    LinearClamped,
    NearestRepeated,
    NearestClamped,
    LinearRepeatedAnisotropy,
}

// Asset/Material.hpp's ShadingModel
public enum ShadingModel : uint
{
    Standard = 0,
    Water,
}

// a material asset's parameters, laid out like Asset/Material.hpp's Material. Start from Assets.GetMaterial, change
// what you want and hand it back to Assets.SetMaterial
[StructLayout(LayoutKind.Sequential)]
public struct MaterialData
{
    public Vector4 AlbedoColor;
    public Vector2 UvSize;
    public Vector2 UvOffset;
    public Vector3 EmissiveColor;
    public float RoughnessFactor;
    public float MetallicFactor;
    // glTF's normalTexture.scale and occlusionTexture.strength, both a no-op at 1
    public float NormalScale;
    public float OcclusionStrength;
    public AlphaMode AlphaMode;
    public float AlphaCutoff;
    public SamplingMode SamplingMode;
    uint flipNormalY;
    internal uint Padding;
    uint doubleSided;
    public float TransmissionFactor;
    public float Ior;
    public float ThicknessFactor;
    public Vector3 AttenuationColor;
    // 0 is infinite
    public float AttenuationDistance;
    public ShadingModel ShadingModel;
    public Vector3 WaterScatterColor;
    public Vector2 WaterFlow;
    public float WaterDetailScale;
    public float WaterShoreFade;
    public AssetId AlbedoTexture;
    public AssetId NormalTexture;
    public AssetId EmissiveTexture;
    public AssetId MetallicRoughnessTexture;
    public AssetId OcclusionTexture;
    // factor multiplies the R channel, linear
    public AssetId TransmissionTexture;
    // factor multiplies the G channel, linear
    public AssetId ThicknessTexture;

    // for DirectX convention normal maps
    public bool FlipNormalY
    {
        readonly get => flipNormalY != 0;
        set => flipNormalY = value ? 1u : 0u;
    }

    public bool DoubleSided
    {
        readonly get => doubleSided != 0;
        set => doubleSided = value ? 1u : 0u;
    }
}
