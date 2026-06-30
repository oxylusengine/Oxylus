import common;
import gpu;
import rmvsm;
import scene;
import cull;

[[vk::binding(0)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_table;
[[vk::binding(1)]] StructuredBuffer<u32> dirty_mesh_instance_indices;
[[vk::binding(2)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(4)]] StructuredBuffer<Transform> transforms;
[[vk::binding(5)]] StructuredBuffer<VirtualClipmap> clipmaps;

[[shader("compute")]]
[[numthreads(1, 64, 1)]]
func cs_main(
    u32x3 thread_id: SV_DispatchThreadID,
    uniform VSMContext ctx,
    uniform u32 dirty_mesh_count
) -> void {
    let mesh_index = thread_id.x;
    let linear_tile_index = thread_id.y;
    let clipmap_index = thread_id.z;

    if (mesh_index >= dirty_mesh_count || clipmap_index >= ctx.clipmap_count) {
      return;
    }

    let mesh_instance = mesh_instances[dirty_mesh_instance_indices[mesh_index]];
    let transform = transforms[mesh_instance.transform_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let clipmap = clipmaps[clipmap_index];
    let mvp = mul(clipmap.projection_view_mat, transform.world);

    if (let screen_aabb = project_aabb(mvp, clipmap.z_near, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
        let uv_min = clamp(screen_aabb.min.xy, 0.0, 1.0);
        let uv_max = clamp(screen_aabb.max.xy, 0.0, 1.0);

        if (any(uv_min >= uv_max)) {
            return;
        }

        let limits = i32x4(i32x2(0), i32x2(ctx.page_table_size - 1));
        let page_min = clamp(i32x2(floor(uv_min * f32(ctx.page_table_size))),    limits.xy, limits.zw);
        let page_max = clamp(i32x2(ceil(uv_max * f32(ctx.page_table_size))) - 1, limits.xy, limits.zw);

        let rect_size = page_max - page_min + 1;
        let rect_total = u32(rect_size.x * rect_size.y);

        if (linear_tile_index >= rect_total) {
            return;
        }

        let virt_page = page_min + i32x2(
            i32(linear_tile_index) % rect_size.x,
            i32(linear_tile_index) / rect_size.x
        );

        let wrapped_page = ctx.page_coords_virtual_to_wrapped(virt_page, clipmap.page_offset);
        if (any(wrapped_page == i32x2(INVALID_COORDS))) {
            return;
        }

        let page_address = i32x3(wrapped_page, clipmap_index);
        var page = VSMPageMetadata(page_table.Load(page_address));
        if (page.is_backed()) {
            page.reset();
            page.set_invalidated(true);
            page_table.Store(page_address, page.data);
        }
    }
}
