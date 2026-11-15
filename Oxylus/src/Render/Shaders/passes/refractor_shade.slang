import common;
import gpu;
import scene;
import visbuffer;
import hit;

#include <fullscreen.slang>
#include <transparent_forward_common.slang>

[[vk::binding(14, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(15, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(16, 0)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(17, 0)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(18, 0)]] Texture2D<u32> refractor_visbuffer;
[[vk::binding(19, 0)]] Texture2D<f32> opaque_depth;
[[vk::binding(21, 0)]] Texture2D<f32x4> transmission_source;

[[vk::push_constant]] ForwardPush push;

// stands in for the distance to a background that is only sky
static const f32 SKY_DISTANCE = 100.0;
// 64 pixel texels, coarser levels average so much of the screen that turning the camera changes them
static const f32 MAX_TRANSMISSION_MIP = 6.0;

struct RefractorOutput {
    f32x4 color : SV_Target0;
    f32 reactive : SV_Target1;
};

struct RefractorSurface {
    SurfaceMaterial surface;
    SurfaceLight light;
    f32x3 world_position;
    f32x3 view_direction;
    f32x2 pixel_uv;
    i32x2 pixel;
    f32 depth;
    f32 ior;
    f32 thickness;
    f32 object_scale;
    f32 transmission;
    f32x3 attenuation_color;
    f32 attenuation_distance;
};

struct RefractorResult {
    f32x3 color;
    f32 alpha;
    f32 reactive;
};

// cubic b-spline from four bilinear taps, plain bilinear on the low mips reads as blocks
func sample_transmission_bspline(f32x2 uv, f32 level, u32x2 base_size) -> f32x4 {
    let level_size = max(f32x2(base_size >> u32(level)), f32x2(1.0));
    let texel = uv * level_size - 0.5;
    let origin = floor(texel);
    let f = texel - origin;

    let w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
    let w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;
    let w2 = (1.0 + 3.0 * f + 3.0 * f * f - 3.0 * f * f * f) / 6.0;
    let w3 = f * f * f / 6.0;
    let g0 = w0 + w1;
    let g1 = w2 + w3;
    let h0 = (origin - 0.5 + w1 / g0) / level_size;
    let h1 = (origin + 1.5 + w3 / g1) / level_size;

    let t00 = transmission_source.SampleLevel(linear_clamp_sampler, f32x2(h0.x, h0.y), level);
    let t10 = transmission_source.SampleLevel(linear_clamp_sampler, f32x2(h1.x, h0.y), level);
    let t01 = transmission_source.SampleLevel(linear_clamp_sampler, f32x2(h0.x, h1.y), level);
    let t11 = transmission_source.SampleLevel(linear_clamp_sampler, f32x2(h1.x, h1.y), level);
    return g0.y * (g0.x * t00 + g1.x * t10) + g1.y * (g0.x * t01 + g1.x * t11);
}

func sample_transmission_blurred(f32x2 uv, f32 mip, u32x2 base_size) -> f32x4 {
    let lower = floor(mip);
    let lower_sample = sample_transmission_bspline(uv, lower, base_size);
    let upper_sample = sample_transmission_bspline(uv, lower + 1.0, base_size);
    return lerp(lower_sample, upper_sample, mip - lower);
}

func opaque_depth_at(f32x2 uv) -> f32 {
    let resolution = i32x2(camera.resolution);
    let pixel = clamp(i32x2(uv * camera.resolution), i32x2(0), resolution - 1);
    return opaque_depth.Load(i32x3(pixel, 0));
}

// distance from the surface to the opaque point seen at uv
func background_distance(in RefractorSurface s, f32x2 uv) -> f32 {
    let depth = opaque_depth_at(uv);
    let is_sky = depth <= 0.0;
    // the reversed infinite projection puts the sky at w = 0, which unproject would divide by
    let background_position = camera.unproject_uv(uv, max(depth, 1e-7));
    let distance_behind = min(distance(background_position, s.world_position), SKY_DISTANCE);

    return select(is_sky, SKY_DISTANCE, distance_behind);
}

// the lobe's spread projected from the background back onto the screen, so the blur follows the scene and not
// where the camera points
func lobe_mip(in RefractorSurface s, f32 spread, f32 distance_behind) -> f32 {
    let eye_distance = distance(camera.position.xyz, s.world_position);
    let focal_pixels = 0.5 * camera.resolution.y * abs(camera.projection[1][1]);
    let radius_pixels = spread * focal_pixels * distance_behind / max(eye_distance + distance_behind, 1e-4);
    // the b-spline reaches about two texels into the level it samples
    return log2(max(radius_pixels * 0.5, 1.0));
}

func sample_transmission_rough(f32x2 uv, f32 mip) -> f32x4 {
    var level_count = 1u;
    var width = 0u;
    var height = 0u;
    transmission_source.GetDimensions(0, width, height, level_count);
    let clamped_mip = clamp(mip, 0.0, min(f32(level_count - 1) - 0.001, MAX_TRANSMISSION_MIP));

    return sample_transmission_blurred(uv, clamped_mip, u32x2(width, height));
}

func sample_transmitted(in RefractorSurface s, f32x2 uv) -> f32x4 {
    let roughness = saturate(s.surface.roughness);
    // a medium that doesn't bend light can't scatter it either, the khronos sample viewer's rule
    let spread = roughness * roughness * saturate(s.ior * 2.0 - 2.0);

    return sample_transmission_rough(uv, lobe_mip(s, spread, background_distance(s, uv)));
}

func accept_refracted_uv(in RefractorSurface s, f32x2 uv) -> f32x2 {
    let is_on_screen = all(uv >= 0.0) && all(uv <= 1.0);
    // a nearer opaque surface would smear itself through the refractor
    let is_behind = opaque_depth_at(uv) <= s.depth;

    return select(is_on_screen && is_behind, uv, s.pixel_uv);
}

func refracted_uv(in RefractorSurface s) -> f32x2 {
    let thickness = s.thickness * s.object_scale;
    let exit_direction = refract(-s.view_direction, s.surface.normal, 1.0 / s.ior);
    let exit_position = s.world_position + exit_direction * thickness;
    let exit_clip = camera.jitter_clip(mul(camera.projection_view, f32x4(exit_position, 1.0)));
    let exit_uv = exit_clip.xy / max(exit_clip.w, 1e-6) * 0.5 + 0.5;
    let is_thick = s.thickness > 0.0 && exit_clip.w > 0.0;

    return accept_refracted_uv(s, select(is_thick, exit_uv, s.pixel_uv));
}

func absorption_coefficient(in RefractorSurface s) -> f32x3 {
    let inverse_distance = select(s.attenuation_distance > 0.0, 1.0 / max(s.attenuation_distance, 1e-6), 0.0);
    return -log(max(s.attenuation_color, f32x3(1e-4))) * inverse_distance;
}

func glass_absorption(in RefractorSurface s) -> f32x3 {
    return exp(-absorption_coefficient(s) * s.thickness * s.object_scale);
}

func shade_glass(in RefractorSurface s) -> RefractorResult {
    let N = s.surface.normal;
    let albedo = s.surface.albedo.rgb;
    let metallic = clamp(s.surface.metallic, 0.0, 1.0);

    let NoV = saturate(dot(N, s.view_direction));
    let F0 = lerp(f32x3(dielectric_f0(s.ior)), albedo, metallic);
    let F = F_Schlick(NoV, F0);

    let transmitted_sample = sample_transmitted(s, refracted_uv(s));

    let transmission = s.transmission;
    let transmitted = transmitted_sample.rgb * (1.0 - F) * transmission * albedo * glass_absorption(s);
    let reflection = s.light.specular;
    let diffuse = s.light.diffuse * (1.0 - transmission);

    RefractorResult result;
    let color = reflection + diffuse + transmitted + s.surface.emissive + s.light.aerial_perspective;
    result.color = clamp(com::sanitize(color), f32x3(0.0), f32x3(8000.0));
    let mean_fresnel = dot(F, f32x3(1.0 / 3.0));
    result.alpha = saturate(1.0 - transmission * (1.0 - mean_fresnel) * (1.0 - transmitted_sample.a));
    result.reactive = 0.1 * transmission;

    return result;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> RefractorOutput {
    let pixel = i32x2(input.position.xy);
    let texel = refractor_visbuffer.Load(i32x3(pixel, 0));
    if (texel == ~0u) {
        discard;
    }

    let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
    let tri = resolve_visbuffer_triangle(geometry, meshlet_instances, VisBufferData(texel));
    if (!tri.valid) {
        discard;
    }

    let material = materials[tri.material_index];
    let ior = max(material.get_ior(), 1.0);
    let sample_uv = input.tex_coord - camera.temporalaa_jitter / camera.resolution;
    let decoded = decode_triangle_surface(tri, material, sample_uv * 2.0 - 1.0, camera);

    let world_position = tri.interpolate_position(decoded.triangle.deriv.lambda);
    let depth = refractor_depth.Load(i32x3(pixel, 0));
    let transform = transforms[tri.transform_index];
    let surface = decoded.surface;

    RefractorSurface s = {};
    s.world_position = world_position;
    s.pixel_uv = input.position.xy / camera.resolution;
    s.depth = depth;
    s.surface = surface;
    s.light = light_surface(surface, ior, decoded.triangle.world_normal, world_position, input.position.xy, depth, push);
    s.view_direction = normalize(camera.position.xyz - world_position);
    s.pixel = pixel;
    s.ior = ior;
    s.thickness = material.sample_thickness(decoded.uv_grad);
    s.object_scale = (length(transform.world[0].xyz) + length(transform.world[1].xyz) + length(transform.world[2].xyz)) / 3.0;
    s.transmission = material.sample_transmission(decoded.uv_grad) * (1.0 - clamp(surface.metallic, 0.0, 1.0));
    s.attenuation_color = material.get_attenuation_color();
    s.attenuation_distance = material.get_attenuation_distance();

    // water shades as glass until it gets its own model
    let shaded = shade_glass(s);

    let background = transmission_source.Load(i32x3(pixel, 0));
    let is_blended = HAS_FLAG(material.flags, MaterialFlag::AlphaBlend);
    let coverage = select(is_blended, saturate(surface.albedo.a), 1.0);

    RefractorOutput output;
    output.color = f32x4(lerp(background.rgb, shaded.color, coverage), lerp(background.a, shaded.alpha, coverage));
    output.reactive = shaded.reactive;

    return output;
}
