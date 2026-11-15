module hit;

import common;
import gpu;
import scene;
import visbuffer;
import pbr;

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
    var mesh = geometry.meshes[mesh_instance.mesh_index];
    mesh.apply_skin(mesh_instance);
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
    var mesh = geometry.meshes[mesh_instance.mesh_index];
    mesh.apply_skin(mesh_instance);
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
        surface.normal = frame.to_world(material.sample_tangent_normal(uv_sample));
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

public struct TriangleSample {
    public PartialDeriv deriv;
    public f32 normal_sign;
    public f32x3 world_normal;
};

public func sample_triangle(
    in TriangleHit tri, in Material material, f32x2 ndc, in Camera camera
) -> TriangleSample {
    TriangleSample result;
    result.deriv = compute_partial_derivatives(tri.world_positions, ndc, camera.resolution, camera.projection_view);

    let is_flipped = result.deriv.is_back_facing && material.is_double_sided();
    result.normal_sign = select(is_flipped, -1.0, 1.0);
    result.world_normal = result.normal_sign * tri.interpolate_normal(result.deriv.lambda);

    return result;
}

public struct DecodedSurface {
    public SurfaceMaterial surface;
    public TriangleSample triangle;
    public TangentFrame frame;
    public UVGradient uv_grad;
};

public func decode_triangle_surface(
    in TriangleHit tri, in Material material, f32x2 ndc, in Camera camera
) -> DecodedSurface {
    DecodedSurface result;
    result.triangle = sample_triangle(tri, material, ndc, camera);
    let deriv = result.triangle.deriv;
    let normal_sign = result.triangle.normal_sign;
    let world_normal = result.triangle.world_normal;

    var tex_coord_grad = deriv.gradient_of(tri.tex_coords);
    material.transform_uv(tex_coord_grad);

    const f32x3x3 camera_relative_world_positions = {
        tri.world_positions[0].xyz - camera.position.xyz,
        tri.world_positions[1].xyz - camera.position.xyz,
        tri.world_positions[2].xyz - camera.position.xyz,
    };

    let position_dx = mul(deriv.ddx, camera_relative_world_positions);
    let position_dy = mul(deriv.ddy, camera_relative_world_positions);
    let uv_dx = tex_coord_grad.ddx;
    let uv_dy = tex_coord_grad.ddy;
    const TangentFrame frame = build_tangent_frame(world_normal, position_dx, position_dy, uv_dx, uv_dy);

    // biased after the tangent frame, which needs the true uv derivatives
    tex_coord_grad.apply_mip_bias(camera.texture_mip_bias);
    var surface = shade_surface(material, frame, tex_coord_grad);

    if (surface.roughness < 1.0) {
        // evaluate this triangle's footprint explicitly: screen quad derivatives can cross
        // unrelated triangles or discarded pixels in a visibility buffer decoder
        f32x3 normal_derivatives[2];
        [ForceUnroll]
        for (i32 axis = 0; axis < 2; axis++) {
            let bary_offset = axis == 0 ? deriv.ddx : deriv.ddy;
            let interpolated_normal = normal_sign * mul(deriv.lambda + bary_offset, tri.world_normals);
            let normal_length_sq = dot(interpolated_normal, interpolated_normal);
            let vertex_normal = normal_length_sq > 1e-8 ? interpolated_normal * rsqrt(normal_length_sq) : world_normal;
            var sample_normal = vertex_normal;
            if (material.flags & MaterialFlag::HasNormalImage) {
                var normal_sample = tex_coord_grad;
                // the footprint stays one pixel wide even when texture lod is biased
                normal_sample.uv += axis == 0 ? uv_dx : uv_dy;
                let sample_frame = build_tangent_frame(vertex_normal, position_dx, position_dy, uv_dx, uv_dy);
                sample_normal = sample_frame.to_world(material.sample_tangent_normal(normal_sample));
            }
            normal_derivatives[axis] = sample_normal - surface.normal;
        }
        surface.roughness = filter_specular_roughness(surface.roughness, normal_derivatives[0], normal_derivatives[1]);
    }

    result.surface = surface;
    result.frame = frame;
    result.uv_grad = tex_coord_grad;

    return result;
}
