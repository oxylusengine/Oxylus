module meshlet;

import common;
import gpu;
import scene;
import cull;

#include <defines.slang>

public struct MeshletTaskPayload {
    public u32 base_meshlet_instance_index;
    public u8 visible_indices[CULLING_MESHLET_COUNT];
};

public func mesh_primitive_id() -> u32 {
    return spirv_asm {
        OpCapability MeshShadingEXT;
        OpExtension "SPV_EXT_mesh_shader";
        OpDecorate builtin(PrimitiveId:u32) Flat;
        result:$$u32 = OpLoad builtin(PrimitiveId:u32);
    };
}

public struct MeshletSource {
    public MeshInstance mesh_instance;
    public Mesh mesh;
    public MeshLOD mesh_lod;
    public Meshlet meshlet;
    public TransformWorld transform;
};

public func resolve_meshlet_source(
    StructuredBuffer<Mesh> meshes,
    StructuredBuffer<MeshInstance> mesh_instances,
    StructuredBuffer<TransformWorld> transforms,
    in MeshletInstance meshlet_instance
) -> MeshletSource {
    MeshletSource source;
    source.mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
    source.mesh = meshes[source.mesh_instance.mesh_index];
    source.mesh.apply_skin(source.mesh_instance);
    source.mesh_lod = source.mesh.lods[source.mesh_instance.lod_index];
    source.meshlet = source.mesh_lod.meshlets[meshlet_instance.meshlet_index];
    source.transform = transforms[source.mesh_instance.transform_index];

    return source;
}

public func meshlet_triangle_corners(in MeshletSource source, u32 local_index) -> u32x3 {
    let base_triangle_index = source.meshlet.local_triangle_index_offset + local_index * 3;

    return u32x3(
        Meshlet.get_micro_index(source.mesh_lod.local_triangle_indices, base_triangle_index + 0),
        Meshlet.get_micro_index(source.mesh_lod.local_triangle_indices, base_triangle_index + 1),
        Meshlet.get_micro_index(source.mesh_lod.local_triangle_indices, base_triangle_index + 2)
    );
}

public func is_meshlet_triangle_culled(in f32x4x3 clip_pos, bool is_double_sided) -> bool {
    let in_front = clip_pos[0].z >= 0.0 && clip_pos[1].z >= 0.0 && clip_pos[2].z >= 0.0;

    return !in_front || (!is_double_sided && test_triangle_backface(clip_pos));
}

public struct MeshletTaskList {
    public u32 count;
    public u32 offset;
};

public func meshlet_task_list(
    u32 cull_flags,
    u32 meshlet_instance_capacity,
    StructuredBuffer<MeshletInstanceVisibility> visibility
) -> MeshletTaskList {
    let is_transparent = HAS_FLAG(cull_flags, CullFlag.Transparent);
    let is_refractor_list = HAS_FLAG(cull_flags, CullFlag.RefractorList);
    let transparent_count = select(is_refractor_list, visibility[0].total_refractor_meshlet_instances, visibility[0].total_blend_meshlet_instances);

    MeshletTaskList list;
    list.count = select(is_transparent, transparent_count, visibility[0].total_visible_meshlet_instances);
    list.offset = select(is_transparent && is_refractor_list, meshlet_instance_capacity - list.count, 0u);

    return list;
}

public struct MeshletCullResources {
    public Texture2D<f32> hiz_image;
    public StructuredBuffer<Mesh> meshes;
    public StructuredBuffer<MeshInstance> mesh_instances;
    public StructuredBuffer<MeshletInstance> meshlet_instances;
    public StructuredBuffer<TransformWorld> transforms;
    public StructuredBuffer<Material> materials;
    public CullCamera camera;
};

public func is_task_meshlet_visible(
    in MeshletCullResources res,
    u32 cull_flags,
    u32 meshlet_instance_index,
    bool was_visible
) -> bool {
    let is_transparent = HAS_FLAG(cull_flags, CullFlag.Transparent);
    let meshlet_instance = res.meshlet_instances[meshlet_instance_index];
    let mesh_instance = res.mesh_instances[meshlet_instance.mesh_instance_index];
    let transform = res.transforms[mesh_instance.transform_index];
    let mvp = mul(res.camera.projection_view, transform.world);

    var mesh = res.meshes[mesh_instance.mesh_index];
    mesh.apply_skin(mesh_instance);
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let meshlet_bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];
    let aabb_center = meshlet_bounds.get_aabb_center();
    let aabb_extent = meshlet_bounds.get_aabb_extent();
    let cone_cutoff = meshlet_bounds.get_cone_cutoff();

    let cone_axis = normalize(transform.to_world_normal(meshlet_bounds.get_cone_axis()));
    let world_center = transform.to_world_position(aabb_center).xyz;
    let world_radius = transform.to_world_radius(length(aabb_extent * 0.5));

    // meshlet AABBs and cones describe the bind pose, so on a skinned instance they are simply
    // wrong, and per-mesh culling already used the animated bounds so every meshlet passes here
    let skinned = mesh_instance.is_skinned();
    let is_double_sided = res.materials[mesh_instance.material_index].is_double_sided();

    var visible = HAS_FLAG(cull_flags, CullFlag.LatePass) ? true : was_visible;
    visible = visible && (skinned || is_double_sided || cone_cutoff >= 1.0 || !test_cone(world_center, world_radius, cone_axis, cone_cutoff, res.camera.position));
    visible = visible && (skinned || test_frustum(mvp, aabb_center, aabb_extent));

    if ((HAS_FLAG(cull_flags, CullFlag.TestOcclusion | CullFlag.LatePass) || is_transparent) && visible && !skinned) {
        if (let screen_aabb = project_aabb(mvp, res.camera.near_clip, aabb_center, aabb_extent)) {
            visible = !test_occlusion(screen_aabb, res.hiz_image);
        }
    }

    return visible;
}

public struct MeshletVertex {
    public f32x3 world_position;
    public f32x3 world_normal;
    public f32x2 tex_coord;
};

public func fetch_meshlet_vertex(in MeshletSource source, u32 vertex_index) -> MeshletVertex {
    MeshletVertex vertex;
    vertex.world_position = source.transform.to_world_position(source.meshlet.position(source.mesh, vertex_index)).xyz;
    vertex.world_normal = source.transform.to_world_normal(source.mesh.decode_normal(vertex_index));
    vertex.tex_coord = source.meshlet.tex_coord(source.mesh, vertex_index);

    return vertex;
}
