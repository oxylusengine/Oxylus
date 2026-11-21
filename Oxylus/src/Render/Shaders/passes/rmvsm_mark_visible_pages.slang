import common;
import gpu;
import scene;
import rmvsm;
import visbuffer;
import hit;

[[vk::constant_id(0)]] const bool IS_TRANSPARENT = false;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<VirtualClipmap> clipmaps;

[[vk::binding(2, 0)]] Texture2D<f32> depth_image;
[[vk::binding(3, 0)]] Texture2D<f32x4> normal_image;

[[vk::binding(4, 0)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables;
[[vk::binding(5, 0)]] RWStructuredBuffer<u32> page_occupancy;
[[vk::binding(6, 0)]] RWStructuredBuffer<VSMPageAllocator> allocator;

[[vk::binding(7, 0)]] Texture2D<u32> refractor_visbuffer;
[[vk::binding(8, 0)]] Texture2D<f32> opaque_depth_image;
[[vk::binding(9, 0)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(10, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(11, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(12, 0)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(13, 0)]] StructuredBuffer<Material> materials;

groupshared VirtualClipmap shared_clipmaps[10];

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    u32 group_index : SV_GroupIndex,
    uniform VSMContext ctx
) -> void {
    if (group_index < 10) {
        shared_clipmaps[group_index] = clipmaps[group_index];
    }

    GroupMemoryBarrierWithGroupSync();

    if (any(thread_id >= u32x2(ctx.depth_extent))) {
        return;
    }

    let depth = depth_image.Load(i32x3(thread_id, 0));
    if (depth == 0.0) {
        return;
    }

    let uv = (f32x2(thread_id.xy) + 0.5) / f32x2(ctx.depth_extent);
    let world_pos = camera.unproject_uv(uv, depth);
    var flat_normal = f32x3(0.0, 1.0, 0.0);
    if (IS_TRANSPARENT) {
        let texel = refractor_visbuffer.Load(i32x3(thread_id, 0));
        if (texel == ~0u || depth == opaque_depth_image.Load(i32x3(thread_id, 0))) {
            return;
        }

        let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
        let tri = resolve_visbuffer_triangle(geometry, meshlet_instances, VisBufferData(texel));
        if (!tri.valid) {
            return;
        }

        let sample_uv = uv - camera.temporalaa_jitter / camera.resolution;
        flat_normal = sample_triangle(tri, materials[tri.material_index], sample_uv * 2.0 - 1.0, camera).world_normal;
    } else {
        let mapped_smooth_normal = normal_image.Load(i32x3(thread_id, 0));
        flat_normal = normalize(com::oct_to_vec3(mapped_smooth_normal.ba));
    }

    let clipmap_selection = ctx.get_clipmap_selection(camera, uv, depth, flat_normal);
    for (var selection_index = 0u; selection_index < 2u; selection_index += 1u) {
        let clipmap_index = selection_index == 0u
            ? clipmap_selection.fine_clipmap_index
            : clipmap_selection.coarse_clipmap_index;
        if (selection_index == 1u && clipmap_index == clipmap_selection.fine_clipmap_index) {
            continue;
        }

        let clipmap = shared_clipmaps[clipmap_index];
        let clipmap_info = clipmap.get_clipmap_info(world_pos);
        let virt_coords = clipmap_info.get_virt_coords(ctx.get_page_table_extent().xy);
        let wrapped_coords = ctx.page_coords_virtual_to_wrapped(virt_coords, clipmap.page_offset);
        let page_table_address = i32x3(wrapped_coords, i32(clipmap_index));

        // scalarization loop in listing 3.3
        var thread_active = all(wrapped_coords >= 0);
        var first_thread_visible = false;
        while (thread_active) {
            let subgroup_address = WaveReadLaneFirst(page_table_address);
            if (all(subgroup_address == page_table_address)) {
                if (WaveIsFirstLane()) {
                    first_thread_visible = true;
                }
                thread_active = false;
            }
        }

        if (first_thread_visible) {
            let prev_page_data = __atomic_or(page_tables[page_table_address], u32(VSMPageState.Visible), MemoryOrder::Relaxed);
            let prev_page = VSMPageMetadata(prev_page_data);

            if (!prev_page.is_visible()) {
                // page became visible this frame

                if (prev_page.is_backed()) {
                    // we already have allocated this page, and it was marked visible
                    // keep track of visible and backed pages so the allocator wont overwrite it
                    let physical_address = prev_page.get_physical_address();
                    page_occupancy[physical_address] = 1u;
                } else {
                    let alloc_request = VSMAllocRequest(page_table_address, -1);
                    push_page_alloc_request(allocator[0], alloc_request);
                }
            }
        }
    }
}
