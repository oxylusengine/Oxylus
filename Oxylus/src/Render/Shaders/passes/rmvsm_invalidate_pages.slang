import common;
import gpu;
import rmvsm;
import scene;
import cull;

#include <defines.slang>

[[vk::binding(0)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_table;
[[vk::binding(1)]] StructuredBuffer<u32> dirty_mesh_instance_indices;
[[vk::binding(2)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(4)]] StructuredBuffer<Transform> transforms;
[[vk::binding(5)]] StructuredBuffer<VirtualClipmap> clipmaps;

func invalidate_footprint(
    f32x4x4 mvp,
    f32 near,
    f32x3 aabb_center,
    f32x3 aabb_extent,
    u32 lane_index,
    i32 clipmap_index,
    VSMContext ctx,
    VirtualClipmap clipmap
) -> void {
    if (let screen_aabb = project_aabb(mvp, near, aabb_center, aabb_extent)) {
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

        for (var i = lane_index; i < rect_total; i = i + INVALIDATE_PAGES_WORKGROUP_Y) {
            let virt_page = page_min + i32x2(
                i32(i) % rect_size.x,
                i32(i) / rect_size.x
            );

            let wrapped_page = ctx.page_coords_virtual_to_wrapped(virt_page, clipmap.page_offset);
            if (any(wrapped_page == i32x2(INVALID_COORDS))) {
                continue;
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
}

[[shader("compute")]]
[[numthreads(1, INVALIDATE_PAGES_WORKGROUP_Y, 1)]]
func cs_main(
    u32x3 thread_id: SV_DispatchThreadID,
    uniform VSMContext ctx
) -> void {
    let mesh_index = thread_id.x;
    let lane_index = thread_id.y;
    let clipmap_index = thread_id.z;

    if (clipmap_index >= ctx.clipmap_count) {
      return;
    }

    let mesh_instance = mesh_instances[dirty_mesh_instance_indices[mesh_index]];
    let transform = transforms[mesh_instance.transform_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let clipmap = clipmaps[clipmap_index];

    let mvp_prev = mul(clipmap.projection_view_mat, transform.previous_world);
    invalidate_footprint(mvp_prev, clipmap.z_near, mesh.bounds.aabb_center, mesh.bounds.aabb_extent, lane_index, i32(clipmap_index), ctx, clipmap);

    let mvp_curr = mul(clipmap.projection_view_mat, transform.world);
    invalidate_footprint(mvp_curr, clipmap.z_near, mesh.bounds.aabb_center, mesh.bounds.aabb_extent, lane_index, i32(clipmap_index), ctx, clipmap);
}
