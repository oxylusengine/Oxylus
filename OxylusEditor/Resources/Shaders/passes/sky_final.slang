module sky_view;

import common;
import gpu;
import sky;
import scene;

struct ShaderParameters {
    Image2D<f32x4> sky_transmittance_lut;
    Image3D<f32x4> sky_aerial_perspective_lut;
    Image2D<f32x4> sky_view_lut;
    Image2D<f32> depth_image;
    ConstantBuffer<Atmosphere> atmosphere;
    ConstantBuffer<Sun> sun;
    ConstantBuffer<Camera> camera;
};

ParameterBlock<ShaderParameters> params;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID) -> VertexOutput{
    VertexOutput output;
    output.tex_coord = f32x2((vertex_index << 1) & 2, vertex_index & 2);
    output.position = f32x4(2.0 * output.tex_coord - 1.0, 1.0, 1.0);

    return output;
}

func draw_sun(f32x3 world_dir, f32x3 sun_dir, f32 radius) -> f32x3 {
    const let min_cos_theta = cos(radius * PI / 180.0);

    const let cosTheta = dot(world_dir, sun_dir);
    if (cosTheta >= min_cos_theta) {
        return 1.0;
    }

    const let offset = min_cos_theta - cosTheta;
    const let gaussianBloom = exp(-offset * 50000.0) * 0.5;
    const let invBloom = 1.0/(0.02 + offset * 300.0) * 0.01;
    return f32x3(gaussianBloom + invBloom);
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
    f32 depth = params.depth_image.sample_mip(material_samplers[1], input.tex_coord, 0.0);
    f32x3 NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    f32x4 world_pos = mul(params.camera.inv_projection_view, f32x4(NDC, 1.0));
    f32x3 clip_pos = world_pos.xyz / world_pos.w;
    f32x3 camera_pos = params.camera.position.xyz;

    if (depth != 1.0) {
        f32x3 camera_relative_pos = (clip_pos - camera_pos) * CAMERA_SCALE_UNIT;
        return sample_aerial_perspective(
            params.atmosphere, params.sky_aerial_perspective_lut, material_samplers[1], input.tex_coord, camera_relative_pos);
    }

    f32x3 eye_dir = normalize(clip_pos - params.camera.position.xyz);
    f32x3 eye_pos = params.atmosphere.eye_pos;
    f32 h = length(eye_pos);
    f32x3 up = f32x3(0.0, 1.0, 0.0);
    f32x3 right = normalize(cross(up, eye_dir));
    f32x3 forward = normalize(cross(right, up));
    const f32x3 sun_dir = normalize(params.sun.direction);
    f32x2 light_on_plane = normalize(f32x2(dot(sun_dir, forward), dot(sun_dir, right)));

    f32 view_zenith_cos_angle = dot(eye_dir, up);

    const let planet_intersection = com::ray_sphere_intersect_nearest(eye_pos, eye_dir, params.atmosphere->planet_radius);
    f32x2 uv = sky_view_params_to_lut_uv(params.atmosphere, planet_intersection.hasValue, h, view_zenith_cos_angle, light_on_plane);
    f32x4 result = params.sky_view_lut.sample_mip(material_samplers[1], uv, 0.0);
    f32x3 luminance = result.rgb;
    f32 transmittance = result.a;

    f32 sun_cos_theta = dot(sun_dir, up);
    f32x2 transmittance_uv = transmittance_params_to_lut_uv(params.atmosphere, f32x2(h, sun_cos_theta));
    f32x3 sun_transmittance = params.sky_transmittance_lut.sample_mip(material_samplers[1], transmittance_uv, 0.0).rgb;

    if (!planet_intersection.hasValue) {
        luminance += draw_sun(eye_dir, params.sun.direction, 1.0) * params.sun.intensity * sun_transmittance;
    }

    return f32x4(luminance, 1.0 - transmittance);
}
