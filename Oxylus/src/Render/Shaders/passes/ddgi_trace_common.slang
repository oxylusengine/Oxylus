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
[[vk::binding(11, 0)]] Texture2D<f32x4> irradiance_atlas;
[[vk::binding(12, 0)]] Texture2D<f32x2> distance_atlas;
[[vk::binding(13, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(14, 0)]] Texture2DArray<u32> pointspot_page_table;
[[vk::binding(15, 0)]] Texture2D<f32> vsm_physical_pages;
[[vk::binding(16, 0)]] RWStructuredBuffer<u32> probe_requests;

static const f32 HIT_TEXTURE_LOD = 3.0;

struct ProbeRayPayload {
    f32x3 radiance;
    f32 hit_distance;
};

func trace_occluded(f32x3 origin, f32x3 direction, f32 t_min, f32 t_max) -> bool {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = t_min;
    ray.TMax = t_max;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene_tlas, RAY_FLAG_NONE, TLAS_MASK_SHADOW_CASTER, ray);
    query.Proceed();

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#include <rt_lighting.slang>

[[shader("miss")]]
[[require(spvRayTracingKHR)]]
func miss_main(inout ProbeRayPayload payload) -> void {
    payload.radiance = miss_radiance(
        constants.atmosphere,
        WorldRayOrigin(),
        WorldRayDirection(),
        constants.sun_direction,
        constants.ambient_color
    );
}

// false on a one sided backface, its distance is stored negated and shading stops there
func record_probe_hit_distance(
    inout ProbeRayPayload payload,
    u32 instance_id,
    u32 primitive_index,
    f32 hit_distance,
    bool is_front_face
) -> bool {
    if (!is_front_face) {
        let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
        let backface_triangle = resolve_ray_triangle(geometry, instance_id, primitive_index);
        var is_two_sided = false;
        if (backface_triangle.valid) {
            is_two_sided = materials[backface_triangle.material_index].is_double_sided();
        }

        if (!is_two_sided) {
            payload.hit_distance = -hit_distance;
            return false;
        }
    }

    payload.hit_distance = hit_distance;
    return true;
}

func shade_probe_hit(
    inout ProbeRayPayload payload,
    u32 instance_id,
    u32 primitive_index,
    f32x2 barycentrics,
    f32 hit_distance,
    f32x3 direction,
    bool is_front_face,
    u32 request_mode
) -> void {
    let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
    let hit = resolve_ray_hit(
        geometry,
        instance_id,
        primitive_index,
        barycentrics,
        is_front_face,
        HIT_TEXTURE_LOD
    );
    if (!hit.valid) {
        return;
    }

    payload.radiance = shade_hit(
        hit,
        constants.volume_count,
        -direction,
        hit_distance,
        constants.sun_direction,
        constants.sun_intensity,
        constants.shadow_ray_offset,
        constants.normal_bias,
        constants.view_bias,
        constants.light_count,
        constants.bounce_valid,
        constants.atmosphere,
        request_mode
    );
}
