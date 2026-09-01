module hit;

import common;
import gpu;
import scene;
import visbuffer;

public struct SceneGeometry {
    public StructuredBuffer<MeshInstance> mesh_instances;
    public StructuredBuffer<Mesh> meshes;
    public StructuredBuffer<TransformWorld> transforms;
    public StructuredBuffer<Material> materials;

    public __init(
        StructuredBuffer<MeshInstance> mesh_instances,
        StructuredBuffer<Mesh> meshes,
        StructuredBuffer<TransformWorld> transforms,
        StructuredBuffer<Material> materials
    ) {
        this.mesh_instances = mesh_instances;
        this.meshes = meshes;
        this.transforms = transforms;
        this.materials = materials;
    }
};

public struct TriangleHit {
    public u32 mesh_instance_index;
    public u32 material_index;
    public u32 transform_index;
    public f32x4x3 world_positions;
    // object space, kept so a consumer can re-transform by the previous frame's matrix for motion vectors
    public f32x3x3 object_positions;
    public f32x3x3 world_normals;
    public f32x2x3 tex_coords;
    public bool valid;

    public func interpolate_position(f32x3 bary) -> f32x3 {
        const f32x3x3 positions = {
            this.world_positions[0].xyz,
            this.world_positions[1].xyz,
            this.world_positions[2].xyz,
        };
        return mul(bary, positions);
    }

    public func interpolate_normal(f32x3 bary) -> f32x3 {
        return normalize(mul(bary, this.world_normals));
    }

    public func interpolate_uv(f32x3 bary) -> f32x2 {
        return mul(bary, this.tex_coords);
    }

    public func face_normal() -> f32x3 {
        let e0 = this.world_positions[1].xyz - this.world_positions[0].xyz;
        let e1 = this.world_positions[2].xyz - this.world_positions[0].xyz;
        return normalize(cross(e0, e1));
    }
};

func build_triangle(
    in SceneGeometry geometry,
    u32 mesh_instance_index,
    in MeshInstance mesh_instance,
    in Mesh mesh,
    in u32x3 indices
) -> TriangleHit {
    TriangleHit hit = {};
    hit.mesh_instance_index = mesh_instance_index;
    hit.material_index = mesh_instance.material_index;
    if (mesh.vertex_count == 0 || any(indices >= mesh.vertex_count)) {
        return hit;
    }

    let transform = geometry.transforms[mesh_instance.transform_index];
    hit.transform_index = mesh_instance.transform_index;
    hit.object_positions = mesh.positions(indices);
    hit.world_positions = transform.to_world_positions(hit.object_positions);
    hit.world_normals = transform.to_world_normals(mesh.normals(indices));
    hit.tex_coords = mesh.tex_coords(indices);
    hit.valid = true;

    return hit;
}

public func resolve_visbuffer_triangle(
    in SceneGeometry geometry,
    StructuredBuffer<MeshletInstance> meshlet_instances,
    in VisBufferData vis
) -> TriangleHit {
    let meshlet_instance = meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = geometry.mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = geometry.meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let meshlet = mesh_lod.meshlets[meshlet_instance.meshlet_index];

    return build_triangle(
        geometry,
        meshlet_instance.mesh_instance_index,
        mesh_instance,
        mesh,
        meshlet.indices(mesh_lod, vis.triangle_index)
    );
}

public func resolve_ray_triangle(in SceneGeometry geometry, u32 mesh_instance_index, u32 primitive_index)
    -> TriangleHit {
    let mesh_instance = geometry.mesh_instances[mesh_instance_index];
    let mesh = geometry.meshes[mesh_instance.mesh_index];
    if (mesh.lod_count == 0) {
        return {};
    }

    let mesh_lod = mesh.lods[0];
    let base = primitive_index * 3;
    if (base + 3 > mesh_lod.indices_count) {
        return {};
    }

    const u32x3 indices = {
        mesh_lod.indices[base + 0],
        mesh_lod.indices[base + 1],
        mesh_lod.indices[base + 2],
    };

    return build_triangle(geometry, mesh_instance_index, mesh_instance, mesh, indices);
}

public struct TangentFrame {
    public f32x3 normal;
    public f32x3 tangent;
    public f32x3 bitangent;

    public func to_world(f32x3 tangent_space) -> f32x3 {
        return normalize(
            tangent_space.x * this.tangent + tangent_space.y * this.bitangent + tangent_space.z * this.normal
        );
    }
};

public func build_tangent_frame(f32x3 normal, f32x3 dp0, f32x3 dp1, f32x2 duv0, f32x2 duv1) -> TangentFrame {
    let dp0_s = dp0 - dot(dp0, normal) * normal;
    let dp1_s = dp1 - dot(dp1, normal) * normal;

    let jacobian_sign = sign(duv0.x * duv1.y - duv0.y * duv1.x);
    var tangent = jacobian_sign * (duv1.y * dp0_s - duv0.y * dp1_s);
    if (jacobian_sign != 0.0) {
        tangent = normalize(tangent);
    }
    let w = jacobian_sign * sign(dot(dp1, cross(normal, dp0)));

    TangentFrame frame;
    frame.normal = normal;
    frame.tangent = tangent;
    frame.bitangent = -w * cross(normal, tangent);

    return frame;
}

public func triangle_tangent_frame(in TriangleHit tri, f32x3 normal) -> TangentFrame {
    return build_tangent_frame(
        normal,
        tri.world_positions[1].xyz - tri.world_positions[0].xyz,
        tri.world_positions[2].xyz - tri.world_positions[0].xyz,
        tri.tex_coords[1] - tri.tex_coords[0],
        tri.tex_coords[2] - tri.tex_coords[0]
    );
}

public struct SurfaceMaterial {
    public f32x4 albedo;
    public f32x3 normal;
    public f32x3 emissive;
    public f32 metallic;
    public f32 roughness;
    public f32 occlusion;
};

public func shade_surface<S : IUVSample>(in Material material, in TangentFrame frame, in S uv_sample)
    -> SurfaceMaterial {
    SurfaceMaterial surface;
    surface.albedo = material.sample_albedo_color(uv_sample);
    surface.emissive = material.sample_emissive_color(uv_sample);

    let metallic_roughness = material.sample_metallic_roughness(uv_sample);
    surface.metallic = metallic_roughness.x;
    surface.roughness = metallic_roughness.y;
    surface.occlusion = material.sample_occlusion_color(uv_sample);
    surface.normal = frame.normal;

    if (material.flags & MaterialFlag::HasNormalImage) {
        var sampled_normal = material.sample_normal_color(uv_sample) * 2.0 - 1.0;

        let two_component = (material.flags & MaterialFlag::NormalTwoComponent) != MaterialFlag::None;
        let flip_y = (material.flags & MaterialFlag::NormalFlipY) != MaterialFlag::None;

        sampled_normal.y = flip_y ? -sampled_normal.y : sampled_normal.y;
        sampled_normal.z = two_component ? sqrt(saturate(1.0 - dot(sampled_normal.xy, sampled_normal.xy)))
                                         : sampled_normal.z;

        surface.normal = frame.to_world(sampled_normal);
    }

    return surface;
}

public struct RayHit {
    public bool valid;
    public u32 mesh_instance_index;
    public u32 material_index;
    public f32x3 position;
    public f32x3 face_normal;
    public f32x3 vertex_normal;
    public f32x2 uv;
    public SurfaceMaterial surface;
};

public func resolve_ray_hit(
    in SceneGeometry geometry,
    u32 mesh_instance_index,
    u32 primitive_index,
    f32x2 barycentrics,
    bool front_face,
    f32 lod
) -> RayHit {
    RayHit hit = {};

    let tri = resolve_ray_triangle(geometry, mesh_instance_index, primitive_index);
    if (!tri.valid) {
        return hit;
    }

    let bary = f32x3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    let material = geometry.materials[tri.material_index];

    var uv_sample = UVLod(tri.interpolate_uv(bary), lod);
    material.transform_uv(uv_sample);

    let sided = front_face ? 1.0 : -1.0;

    hit.valid = true;
    hit.mesh_instance_index = tri.mesh_instance_index;
    hit.material_index = tri.material_index;
    hit.position = tri.interpolate_position(bary);
    hit.face_normal = sided * tri.face_normal();
    hit.vertex_normal = sided * tri.interpolate_normal(bary);
    hit.uv = uv_sample.uv;
    hit.surface = shade_surface(material, triangle_tangent_frame(tri, hit.vertex_normal), uv_sample);

    return hit;
}
