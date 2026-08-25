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

public constexpr static f32 VSM_CLIPMAP_TRANSITION_WIDTH = 0.25;

public struct VSMClipmapSelection {
    public f32 level_f;
    public u32 fine_clipmap_index;
    public u32 coarse_clipmap_index;
    public f32 blend_weight;

    public __init(f32 level_f, u32 fine_clipmap_index, u32 coarse_clipmap_index, f32 blend_weight) {
        this.level_f = level_f;
        this.fine_clipmap_index = fine_clipmap_index;
        this.coarse_clipmap_index = coarse_clipmap_index;
        this.blend_weight = blend_weight;
    }
};

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
        return (this.first_clipmap_width * 2.0) / this.virtual_extent;
    }

    public func intersect_pixel_ray_with_plane(
        in Camera camera,
        f32x2 uv,
        f32x3 plane_position,
        f32x3 plane_normal
    ) -> f32x3 {
        let ray_near = camera.unproject_uv(uv, 1.0);
        let ray_far = camera.unproject_uv(uv, 0.0);
        let ray_direction = normalize(ray_far - ray_near);
        let denominator = dot(plane_normal, ray_direction);
        let safe_denominator = denominator < 0.0
            ? min(denominator, -1e-5)
            : max(denominator, 1e-5);
        let distance_to_plane = dot(plane_normal, plane_position - ray_near) / safe_denominator;
        return ray_near + ray_direction * distance_to_plane;
    }

    public func get_clipmap_level_f(
        in Camera camera,
        f32x2 uv,
        f32 depth,
        f32x3 flat_normal
    ) -> f32 {
        let center_position = camera.unproject_uv(uv, depth);
        let inv_resolution = 1.0 / camera.resolution;
        let uv_offset = inv_resolution * 0.5;

        let bottom_left = this.intersect_pixel_ray_with_plane(
            camera, uv + f32x2(-uv_offset.x, uv_offset.y), center_position, flat_normal
        );
        let bottom_right = this.intersect_pixel_ray_with_plane(
            camera, uv + f32x2(uv_offset.x, uv_offset.y), center_position, flat_normal
        );
        let top_left = this.intersect_pixel_ray_with_plane(
            camera, uv + f32x2(-uv_offset.x, -uv_offset.y), center_position, flat_normal
        );
        let top_right = this.intersect_pixel_ray_with_plane(
            camera, uv + f32x2(uv_offset.x, -uv_offset.y), center_position, flat_normal
        );

        let footprint_width = max(distance(bottom_left, bottom_right), distance(top_left, top_right));
        let footprint_height = max(distance(bottom_left, top_left), distance(bottom_right, top_right));
        let footprint_size = max(footprint_width, footprint_height);
        let first_clipmap_texel_length = max(this.get_first_clipmap_texel_length(), 1e-5);
        let quality_level_f = log2(max(footprint_size, first_clipmap_texel_length * 1e-5) /
                                   first_clipmap_texel_length) + this.clipmap_selection_bias;

        let page_count = f32(this.page_table_size);
        let scale_ratio = (page_count - 2.0) / page_count;
        let safe_half_extent = max(this.first_clipmap_width * scale_ratio, 1e-5);
        let camera_distance = length(center_position - camera.position.xyz);
        let coverage_level_f = ceil(log2(max(camera_distance / safe_half_extent, 1e-5)));
        let min_coverage_level = max(coverage_level_f, 0.0);

        let finite_quality_level_f = isfinite(quality_level_f) ? quality_level_f : min_coverage_level;
        return max(max(finite_quality_level_f, min_coverage_level), 0.0);
    }

    public func get_clipmap_selection(
        in Camera camera,
        f32x2 uv,
        f32 depth,
        f32x3 flat_normal
    ) -> VSMClipmapSelection {
        let level_f = this.get_clipmap_level_f(camera, uv, depth, flat_normal);
        let max_clipmap_index = u32(this.clipmap_count - 1);
        let clamped_level_f = clamp(level_f, 0.0, f32(max_clipmap_index));
        let level_fraction = fract(clamped_level_f);

        var fine_clipmap_index = u32(floor(clamped_level_f));
        var coarse_clipmap_index = fine_clipmap_index;
        var blend_weight = 0.0;
        if (level_fraction > 0.0 && fine_clipmap_index < max_clipmap_index) {
            coarse_clipmap_index = fine_clipmap_index + 1u;
            if (level_fraction < VSM_CLIPMAP_TRANSITION_WIDTH) {
                blend_weight = smoothstep(0.0, VSM_CLIPMAP_TRANSITION_WIDTH, level_fraction);
            } else {
                fine_clipmap_index = coarse_clipmap_index;
            }
        }

        return VSMClipmapSelection(
            clamped_level_f,
            fine_clipmap_index,
            coarse_clipmap_index,
            blend_weight
        );
    }

    public func get_clipmap_index(in Camera camera, f32x2 uv, f32 depth, f32x3 flat_normal) -> u32 {
        return this.get_clipmap_selection(camera, uv, depth, flat_normal).coarse_clipmap_index;
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
