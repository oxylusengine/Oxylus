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
[[vk::binding(4)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(5)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(6)]] StructuredBuffer<TransformPrevious> transforms_previous;
[[vk::binding(7)]] StructuredBuffer<MeshBounds> removed_bounds;

constexpr static u32 INVALIDATE_PAGE_BLOCK_RESOLUTION = 8;

func invalidate_in_section(
    f32x4x4 mvp,
    f32 near,
    f32x3 aabb_center,
    f32x3 aabb_extent,
    i32x2 section_min,
    i32x2 section_max,
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

        let intersect_min = max(page_min, section_min);
        let intersect_max = min(page_max, section_max);
        if (any(intersect_min > intersect_max)) {
            return;
        }

        for (var y = intersect_min.y; y <= intersect_max.y; y++) {
            for (var x = intersect_min.x; x <= intersect_max.x; x++) {
                let wrapped_page = ctx.page_coords_virtual_to_wrapped(i32x2(x, y), clipmap.page_offset);
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
}

func section_range(u32 section_index, VSMContext ctx, out i32x2 section_min, out i32x2 section_max) -> bool {
    let sections_per_dim = ctx.page_table_size / i32(INVALIDATE_PAGE_BLOCK_RESOLUTION);
    let section_x = i32(section_index) % sections_per_dim;
    let section_y = i32(section_index) / sections_per_dim;
    section_min = i32x2(section_x, section_y) * i32(INVALIDATE_PAGE_BLOCK_RESOLUTION);
    section_max = section_min + i32(INVALIDATE_PAGE_BLOCK_RESOLUTION - 1);

    return section_x < sections_per_dim && section_y < sections_per_dim;
}

[[shader("compute")]]
[[numthreads(1, INVALIDATE_PAGES_WORKGROUP_Y, 1)]]
func cs_main(
    u32x3 thread_id: SV_DispatchThreadID,
    uniform VSMContext ctx
) -> void {
    let mesh_index = thread_id.x;
    let section_index = thread_id.y;
    let clipmap_index = thread_id.z;

    if (clipmap_index >= ctx.clipmap_count) {
      return;
    }

    var section_min = i32x2(0);
    var section_max = i32x2(0);
    if (!section_range(section_index, ctx, section_min, section_max)) {
      return;
    }

    let mesh_instance = mesh_instances[dirty_mesh_instance_indices[mesh_index]];
    let transform = transforms[mesh_instance.transform_index];
    let transform_previous = transforms_previous[mesh_instance.transform_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let clipmap = clipmaps[clipmap_index];

    let mvp_prev = mul(clipmap.projection_view_mat, transform_previous.previous_world);
    invalidate_in_section(
      mvp_prev, clipmap.z_near,
      mesh.bounds.aabb_center, mesh.bounds.aabb_extent,
      section_min, section_max,
      i32(clipmap_index), ctx, clipmap
    );

    let mvp_curr = mul(clipmap.projection_view_mat, transform.world);
    invalidate_in_section(
      mvp_curr, clipmap.z_near,
      mesh.bounds.aabb_center, mesh.bounds.aabb_extent,
      section_min, section_max,
      i32(clipmap_index), ctx, clipmap
    );
}

// meshes destroyed since the last frame are in no instance buffer anymore, so the pages their shadow
// was drawn into are found from their last world box instead
[[shader("compute")]]
[[numthreads(1, INVALIDATE_PAGES_WORKGROUP_Y, 1)]]
func cs_bounds(
    u32x3 thread_id: SV_DispatchThreadID,
    uniform VSMContext ctx
) -> void {
    let bounds_index = thread_id.x;
    let section_index = thread_id.y;
    let clipmap_index = thread_id.z;

    if (clipmap_index >= ctx.clipmap_count) {
      return;
    }

    var section_min = i32x2(0);
    var section_max = i32x2(0);
    if (!section_range(section_index, ctx, section_min, section_max)) {
      return;
    }

    let bounds = removed_bounds[bounds_index];
    let clipmap = clipmaps[clipmap_index];
    invalidate_in_section(
      clipmap.projection_view_mat, clipmap.z_near,
      bounds.aabb_center, bounds.aabb_extent,
      section_min, section_max,
      i32(clipmap_index), ctx, clipmap
    );
}
