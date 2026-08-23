//
// Resolution Matched Virtual Shadowmaps by Matěj Sakmary (https://github.com/MatejSakmary).
//
// This implementation is an amalgamation of multiple other RM/S VSM implementations:
// Mainly with improvements made by:
// - Jake Ryan (https://github.com/JuanDiegoMontoya),
// - J Stephano (https://ktstephano.github.io/rendering/stratusgfx/svsm),
// - LVSTRI (https://github.com/LVSTRI)
//

import common;
import scene;

public constexpr static i32 INVALID_COORDS = -1;

[Flags]
public enum VSMPageState : u32 {
    // when set, this page will be sampled and requires backing
    Visible,
    // when set, this page will need to be redrawn this frame
    Dirty,
    // when set, the entry is backed by a physical page
    Backed,
    // DEBUG: Is dynamically invalidated
    Invalidated,
    // DEBUG: Is allocation failed
    AllocationFailed,
};

public constexpr static u32 VSM_METADATA_BITS = 16u;
public constexpr static u32 VSM_METADATA_MASK = (1u << VSM_METADATA_BITS) - 1u;

public constexpr static u32 VSM_PAGE_PHYSICAL_ADDRESS_BITS = 16u;
public constexpr static u32 VSM_PAGE_PHYSICAL_ADDRESS_MASK = (1u << VSM_PAGE_PHYSICAL_ADDRESS_BITS) - 1u;

public struct VSMPageMetadata {
    public u32 data;

    [mutating]
    public __init(u32 data) {
        this.data = data;
    }

    [mutating]
    public func reset() -> void {
        this.data = 0;
    }

    public func is_visible() -> bool {
        return (this.data & VSMPageState.Visible) != 0;
    }

    [mutating]
    public func set_visible(bool v) -> void {
        // first we need to clear already set bit, and set the actual value we want
        this.data = (this.data & ~VSMPageState.Visible) | (VSMPageState.Visible * u32(v));
    }

    public func is_dirty() -> bool {
        return (this.data & VSMPageState.Dirty) != 0;
    }

    [mutating]
    public func set_dirty(bool v) -> void {
        this.data = (this.data & ~VSMPageState.Dirty) | (VSMPageState.Dirty * u32(v));
    }

    public func is_backed() -> bool {
        return (this.data & VSMPageState.Backed) != 0;
    }

    [mutating]
    public func set_backed(bool v) -> void {
        this.data = (this.data & ~VSMPageState.Backed) | (VSMPageState.Backed * u32(v));
    }

    public func is_invalidated() -> bool {
        return (this.data & VSMPageState.Invalidated) != 0;
    }

    [mutating]
    public func set_invalidated(bool v) -> void {
        this.data = (this.data & ~VSMPageState.Invalidated) | (VSMPageState.Invalidated * u32(v));
    }

    public func is_alloc_failed() -> bool {
        return (this.data & VSMPageState.AllocationFailed) != 0;
    }

    [mutating]
    public func set_alloc_failed(bool v) -> void {
        this.data = (this.data & ~VSMPageState.AllocationFailed) | (VSMPageState.AllocationFailed * u32(v));
    }

    public func get_physical_address() -> u32 {
        return this.data >> VSM_PAGE_PHYSICAL_ADDRESS_BITS;
    }

    public func get_physical_coords(i32 physical_image_size, i32 page_size) -> u32x2 {
        let physical_address = this.get_physical_address();
        let physical_page_count = physical_image_size / page_size;
        return u32x2(
            physical_address % physical_page_count,
            physical_address / physical_page_count
        );
    }

    [mutating]
    public func set_physical_address(u32 new_physical_address) -> void {
        this.data = (this.data & VSM_PAGE_PHYSICAL_ADDRESS_MASK) | ((new_physical_address & VSM_PAGE_PHYSICAL_ADDRESS_MASK) << 16u);
    }
};

#define VMS_USE_DIAG_PIXEL_FOOTPRINT 1

public struct VSMContext {
    public i32 page_size;
    public i32 page_table_size;
    public i32 physcial_page_table_size;
    public i32 curr_clipmap_index;
    public i32 clipmap_count;
    public i32x2 depth_extent;
    public f32 first_clipmap_width;
    public f32 clipmap_selection_bias;
    public f32 virtual_extent;
    public f32 z_length;
    public f32x3 directional_light_dir;

    public func page_coords_virtual_to_wrapped(i32x2 virt_coords, i32x2 clipmap_offset) -> i32x2 {
        let bounds = i32x2(0, this.page_table_size - 1);
        if (any(virt_coords < bounds.x) || any(virt_coords > bounds.y)) {
            return i32x2(INVALID_COORDS);
        }

        let offset_coords = virt_coords + clipmap_offset;
        let wrapped_coords = com::mod(offset_coords, this.page_table_size);
        return i32x2(wrapped_coords);
    }

    public func get_page_table_extent() -> u32x3 {
        return u32x3(this.page_table_size, this.page_table_size, this.clipmap_count);
    }

    public func get_physical_page_table_extent() -> u32x2 {
        return u32x2(this.physcial_page_table_size);
    }

    public func get_first_clipmap_texel_length() -> f32 {
        // Account for page snapping reducing effective coverage by up to one page per side.
        // See Timberdoodle's scale_ratio: (page_count - 2) / page_count
        let page_count = this.page_table_size;
        let scale_ratio = f32(page_count - 1) / f32(page_count);
        let effective_width = this.first_clipmap_width * scale_ratio;
        return (effective_width * 2.0) / this.virtual_extent;
    }

    public func get_clipmap_level_f(in Camera camera, f32x2 uv, f32 depth) -> f32 {
#if VMS_USE_DIAG_PIXEL_FOOTPRINT
        let inv_resolution = 1.0 / camera.resolution;
        let uv_offset = inv_resolution * 0.5;
        let left = uv + f32x2(-uv_offset.x, uv_offset.y);
        let right = uv + f32x2(uv_offset.x, uv_offset.y);
        let d = distance(
            camera.unproject_uv(left, depth),
            camera.unproject_uv(right, depth)
        );
        let clipmap_level_f = log2(d / this.get_first_clipmap_texel_length());
#else
        let center_pos = camera.unproject_uv(uv, depth);
        let d = length(center_pos - camera.position.xyz);

        let page_count = this.virtual_extent / this.page_size;
        let scale_ratio = f32(page_count - 2) / f32(page_count);
        let base_scale = this.first_clipmap_width * 2.0 * scale_ratio;

        let clipmap_level_f = log2((d / base_scale) * this.clipmap_selection_bias);
#endif
        return max(clipmap_level_f, 0.0);
    }

    public func get_clipmap_index(in Camera camera, f32x2 uv, f32 depth) -> u32 {
        let level_f = this.get_clipmap_level_f(camera, uv, depth);
#if VMS_USE_DIAG_PIXEL_FOOTPRINT
        return min(u32(ceil(this.clipmap_selection_bias + level_f)), u32(this.clipmap_count - 1));
#else
        return clamp(u32(ceil(level_f)), 0u, u32(this.clipmap_count - 1));
#endif
    }
};

public struct VirtualClipmapInfo {
    f32x2 clip_uv;
    public f32 projected_depth;

    public __init(f32x2 clip_uv, f32 projected_depth) {
        this.clip_uv = clip_uv;
        this.projected_depth = projected_depth;
    }

    public func get_virt_coords(i32x2 page_table_extent) -> i32x2 {
        if (any(this.clip_uv < 0.0) | any(this.clip_uv > 1.0)) {
            return i32x2(INVALID_COORDS);
        }

        return i32x2(floor(this.clip_uv * f32x2(page_table_extent)));
    }
};

public struct VirtualClipmap {
    public f32x4x4 projection_view_mat;
    public i32x2 page_offset;
    public f32 z_near;

    public func get_clipmap_info(f32x3 world_pos) -> VirtualClipmapInfo {
        let light_pos_h = mul(this.projection_view_mat, f32x4(world_pos, 1.0));
        let light_pos = light_pos_h.xyz / light_pos_h.w;
        let clip_uv = (light_pos.xy + 1.0) * 0.5;
        let projected_depth = light_pos_h.z / light_pos_h.w;

        return VirtualClipmapInfo(clip_uv, projected_depth);
    }
};

// point lights use six cube-face layers, spot layers follow, VPT mips are LODs

public constexpr static u32 VSM_SPOT_LAYER_OFFSET = MAX_SHADOW_POINT_LIGHTS * 6;
public constexpr static u32 VSM_POINT_SPOT_LAYER_COUNT = VSM_SPOT_LAYER_OFFSET + MAX_SHADOW_SPOT_LIGHTS;

public func vsm_point_layer(i32 point_shadow_index, i32 face_index) -> i32 {
    return point_shadow_index * 6 + face_index;
}

public func vsm_spot_layer(i32 spot_shadow_index) -> i32 {
    return i32(VSM_SPOT_LAYER_OFFSET) + spot_shadow_index;
}

// {+X, -X, +Y, -Y, +Z, -Z}, must match the CPU face matrices
// dir points from the light to the shaded position
public func vsm_point_face_index(f32x3 dir) -> i32 {
    let a = abs(dir);
    let coord_index = a.x < a.y ? (a.y < a.z ? 2 : 1) : (a.x < a.z ? 2 : 0);
    return coord_index * 2 + (dir[coord_index] < 0.0 ? 1 : 0);
}

public struct VSMPointSpotProjection {
    public f32x2 uv;
    public f32 depth;
    public bool valid;
};

public struct VSMPointSpotView {
    public f32x4x4 projection_view;
    public f32x3 light_position;
    public f32 range; // 0 means the layer is inactive
    public u32 light_index;
    public f32 z_near;
    public f32 texel_world_scale;
    public u32 _pad_0;

    public func project(f32x3 world_pos) -> VSMPointSpotProjection {
        let clip_pos = mul(this.projection_view, f32x4(world_pos, 1.0));
        VSMPointSpotProjection result;
        result.valid = clip_pos.w > 0.0;
        let ndc = clip_pos.xyz / max(clip_pos.w, 1e-20);
        result.uv = (ndc.xy + 1.0) * 0.5;
        result.depth = ndc.z;
        result.valid = result.valid && all(result.uv >= 0.0) && all(result.uv <= 1.0);
        return result;
    }
};

public constexpr static u32 VSM_POINT_SPOT_MAX_MESHLET_INSTANCES = 1u << 18;
public constexpr static u32 VSM_POINT_SPOT_MAX_INDEX_COUNT = 1u << 23;

public struct VSMMeshletInstance {
    public u32 mesh_instance_index;
    public u32 meshlet_index;
    public u32 layer;
};

public struct VSMPointSpotContext {
    public i32 curr_mip;
    public u32 layer_count;
    public u32 mesh_instance_count;
    public i32x2 depth_extent;
    public u32 mip_bias_min;
    public u32 shadow_point_light_count;
    public u32 shadow_spot_light_count;
};

public struct VSMAllocRequest {
    public i32x3 page_table_address;
    public i32 mip;
};

public struct VSMPageAllocator {
    public u32 active_request_count;
    public u32 dirty_physical_page_count;
    public u32 free_page_count;
    public u32 alloc_cursor;
    public u32 request_capacity;
    public u32 _pad_0;
    public VSMAllocRequest *requests;
    public u32x2 *dirty_physical_page_coords;
    public u32 *free_page_list;
};

public func push_page_alloc_request(__ref VSMPageAllocator allocator, in VSMAllocRequest request) -> void {
    let index = __atomic_add(allocator.active_request_count, 1, MemoryOrder::AcquireRelease);
    if (index >= allocator.request_capacity) {
        return;
    }
    allocator.requests[index] = request;
}

public func push_dirty_physical_page(__ref VSMPageAllocator allocator, u32x2 physical_page_address) -> void {
    let index = __atomic_add(allocator.dirty_physical_page_count, 1, MemoryOrder::AcquireRelease);
    allocator.dirty_physical_page_coords[index] = physical_page_address;
}

public func reserve_free_page_slots(__ref VSMPageAllocator allocator, u32 count) -> u32 {
    return __atomic_add(allocator.free_page_count, count, MemoryOrder::AcquireRelease);
}

public func pop_free_page_slots(__ref VSMPageAllocator allocator, u32 count) -> u32 {
    return __atomic_add(allocator.alloc_cursor, count, MemoryOrder::AcquireRelease);
}
