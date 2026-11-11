#include <rmvsm_pointspot.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene_tlas;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(2, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(4, 0)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(5, 0)]] StructuredBuffer<Material> materials;
[[vk::binding(6, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(7, 0)]] Texture2D<f32x4> sky_view_lut;
[[vk::binding(8, 0)]] Texture2D<f32x4> sky_transmittance_lut;
[[vk::binding(9, 0)]] Sampler linear_clamp_sampler;
[[vk::binding(10, 0)]] Texture2D<f32x4> irradiance_atlas;
[[vk::binding(11, 0)]] Texture2D<f32x2> distance_atlas;
[[vk::binding(12, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(13, 0)]] Texture2DArray<u32> pointspot_page_table;
[[vk::binding(14, 0)]] Texture2D<f32> vsm_physical_pages;
[[vk::binding(15, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(16, 0)]] Texture2D<f32> half_res_depth;
[[vk::binding(17, 0)]] Texture2D<u32> half_res_face_normal;
[[vk::binding(23, 0)]] RWStructuredBuffer<u32> probe_requests;

static const f32 HIT_TEXTURE_LOD = 3.0;
static const f32 RTGI_ALPHA_LOD = 2.0f;
static const u32 RTGI_NO_HIT = 0xffffffffu;

struct TraceConstants {
    Atmosphere *atmosphere;
    LightGridCell *light_grid;
    VSMPointSpotView *pointspot_views;
    u32 volume_count;
    u32 light_count;
    u32 bounce_valid;
    u32 radiance_atlas_y_offset;
    u32 distance_culling_enabled;
    u32 frame_index;
    u32 ray_list_limit;
    u32 is_guiding_enabled;
    f32 max_distance;
    f32 shadow_ray_offset;
    f32 normal_bias;
    f32 view_bias;
    f32 sun_intensity;
    f32 ao_range;
    f32 guide_concentration;
    f32x3 sun_direction;
    f32x3 ambient_color;
    f32x3 light_grid_origin;
};

[[vk::push_constant]] ConstantBuffer<TraceConstants> constants;

struct RTGIPayload {
    f32 t;
    u32 instance_id;
    u32 primitive_index;
    f32x2 barycentrics;
    u32 is_front_face;
    u32 is_visible;
};

func trace_occluded(f32x3 origin, f32x3 direction, f32 t_min, f32 t_max) -> bool {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = t_min;
    ray.TMax = t_max;

    RTGIPayload shadow = {};
    shadow.is_visible = 0u;
    TraceRay(
        scene_tlas,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        TLAS_MASK_SHADOW_CASTER,
        0,
        0,
        1,
        ray,
        shadow
    );

    return shadow.is_visible == 0u;
}

#include <rt_lighting.slang>

func rtgi_bounce_scale(f32 t) -> f32 {
    let relative_t = min(constants.ao_range, t) / max(constants.ao_range, 1.0e-6);
    return select(constants.ao_range <= 0.0, 1.0, relative_t * relative_t);
}

func shade_ray_hit(in RTGIPayload payload, f32x3 origin, f32x3 direction) -> f32x3 {
    let t = payload.t;
    let is_front_face = payload.is_front_face != 0u;
    let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);

    if (!is_front_face) {
        let material_flags = materials[mesh_instances[payload.instance_id].material_index].flags;
        let is_two_sided = (material_flags & (MaterialFlag::AlphaMask | MaterialFlag::AlphaBlend)) != 0;
        if (!is_two_sided) {
            return f32x3(0.0);
        }
    }

    let is_coarse = constants.distance_culling_enabled != 0 &&
                    constants.bounce_valid != 0 &&
                    t >= probe_volumes[0].max_probe_distance;
    if (is_coarse) {
        let triangle = resolve_ray_triangle(geometry, payload.instance_id, payload.primitive_index);
        if (!triangle.valid) {
            return f32x3(0.0);
        }

        let face_normal = select(is_front_face, 1.0, -1.0) * triangle.face_normal();

        ProbeSampleParams params;
        params.world_position = origin + direction * t;
        params.normal = face_normal;
        params.geometric_normal = face_normal;
        params.view_direction = -direction;
        params.view_distance = t;
        params.specular_direction = face_normal;
        params.want_specular = false;
        params.normal_bias = constants.normal_bias;
        params.view_bias = constants.view_bias;

        let sample = sample_ddgi_radiance(
            probe_volumes,
            probe_states,
            constants.volume_count,
            irradiance_atlas,
            distance_atlas,
            linear_clamp_sampler,
            constants.radiance_atlas_y_offset,
            params,
            probe_requests,
            PROBE_REQUEST_INDIRECT
        );

        return sample.radiance * sample.coverage * rtgi_bounce_scale(t);
    }

    let hit = resolve_ray_hit(
        geometry,
        payload.instance_id,
        payload.primitive_index,
        payload.barycentrics,
        is_front_face,
        HIT_TEXTURE_LOD
    );
    if (!hit.valid) {
        return f32x3(0.0);
    }

    return shade_hit(
        hit,
        constants.volume_count,
        -direction,
        t,
        constants.sun_direction,
        constants.sun_intensity,
        constants.shadow_ray_offset,
        constants.normal_bias,
        constants.view_bias,
        constants.light_count,
        constants.bounce_valid,
        constants.atmosphere,
        PROBE_REQUEST_INDIRECT,
        rtgi_bounce_scale(t)
    );
}

[[shader("miss")]]
[[require(spvRayTracingKHR)]]
func miss_main(inout RTGIPayload payload) -> void {
    payload.t = RTGI_RAY_MISS_T;
}

[[shader("miss")]]
[[require(spvRayTracingKHR)]]
func shadow_miss_main(inout RTGIPayload payload) -> void {
    payload.is_visible = 1u;
}

[[shader("closesthit")]]
[[require(spvRayTracingKHR)]]
func closesthit_main(inout RTGIPayload payload, in BuiltInTriangleIntersectionAttributes attributes) -> void {
    payload.t = RayTCurrent();
    payload.instance_id = InstanceID();
    payload.primitive_index = PrimitiveIndex();
    payload.barycentrics = attributes.barycentrics;
    payload.is_front_face = u32(HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE);
}

[[shader("anyhit")]]
[[require(spvRayTracingKHR)]]
func anyhit_main(inout RTGIPayload payload, in BuiltInTriangleIntersectionAttributes attributes) -> void {
    let mesh_instance = mesh_instances[InstanceID()];
    let material = materials[mesh_instance.material_index];
    if ((material.flags & MaterialFlag::HasAlbedoImage) == 0) {
        return;
    }

    let mesh = meshes[mesh_instance.mesh_index];
    if (mesh.lod_count == 0) {
        return;
    }

    let mesh_lod = mesh.lods[0];
    let base = PrimitiveIndex() * 3;
    if (base + 3 > mesh_lod.indices_count) {
        return;
    }

    const u32x3 indices = {
        mesh_lod.indices[base + 0],
        mesh_lod.indices[base + 1],
        mesh_lod.indices[base + 2],
    };
    if (mesh.vertex_count == 0 || any(indices >= mesh.vertex_count)) {
        return;
    }

    let bary = f32x3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);
    var uv_sample = UVLod(mul(bary, mesh.tex_coords(indices)), RTGI_ALPHA_LOD);
    material.transform_uv(uv_sample);

    let alpha = material.sample_albedo_color(uv_sample).a;
    if (alpha < clamp(material.get_alpha_cutoff(), 0.001, 1.0)) {
        IgnoreHit();
    }
}
