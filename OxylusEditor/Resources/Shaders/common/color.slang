implementing common;

namespace com {
public func rec709_to_xyz(f32x3 color) -> f32x3 {
    f32x3x3 mat = {
        0.4124564, 0.3575761, 0.1804375,
        0.2126729, 0.7151522, 0.0721750,
        0.0193339, 0.1191920, 0.9503041
    };
    return mul(mat, color);
}

public func xyz_to_rec709(f32x3 color) -> f32x3 {
    f32x3x3 mat = {
         3.2404542, -1.5371385, -0.4985314,
        -0.9692660,  1.8760108,  0.0415560,
         0.0556434, -0.2040259,  1.0572252
    };
    return mul(mat, color);
}

public func rec2020_to_xyz(f32x3 color) -> f32x3 {
    f32x3x3 mat = {
        0.636958,  0.1446169, 0.168881,
        0.2627002, 0.6779981, 0.0593017,
        0.0,        0.0280727, 1.0609851
    };
    return mul(mat, color);
}

public func xyz_to_rec2020(f32x3 color) -> f32x3 {
    f32x3x3 mat = {
         1.7166512, -0.3556708, -0.2533663,
        -0.6666844,  1.6164812,  0.0157685,
         0.0176399, -0.0427706,  0.9421031
    };
    return mul(mat, color);
}

public func xyz_to_Yxy(f32x3 color) -> f32x3 {
    f32 inv = 1.0 / dot(color, 1.0);
    return f32x3(color.y, color.xy * inv);
}

public func Yxy_to_xyz(f32x3 color) -> f32x3 {
    return f32x3(color.x * color.y / color.z, color.x, color.x * (1.0 - color.y - color.z) / color.z);
}

public func rec709_to_rec2020(f32x3 color) -> f32x3 {
    return xyz_to_rec2020(rec709_to_xyz(color));
}

public func rec2020_to_rec709(f32x3 color) -> f32x3 {
    return xyz_to_rec709(rec2020_to_xyz(color));
}

public func rec709_oetf(f32x3 color) -> f32x3 {
    bool3 mask = color < 0.018;
    f32x3 a = color * 4.5;
    f32x3 b = 1.099f * pow(color, 0.45) - 0.099;
    return select(mask, a, b);
}

public func rec709_eotf(f32x3 color) -> f32x3 {
    bool3 mask = color < 0.081;
    f32x3 a = color / 4.5f;
    f32x3 b = pow((color + 0.099f) / 1.099, 2.22);
    return select(mask, a, b);
}

public func linear_to_srgb(f32x3 color) -> f32x3 {
    return select(color < 0.0031308, 12.92 * color, 1.055 * pow(abs(color), 1.0 / 2.4) - 0.055);
}

public func srgb_to_linear(f32x3 color) -> f32x3 {
    return select(color < 0.04045, color / 12.92, pow(abs(color + 0.055) / 1.055, 2.4));
}

}