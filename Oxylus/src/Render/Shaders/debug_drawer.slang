module debug_drawer;

import common;
import gpu;

public enum DebugDrawCoord : u32 {
    NDC = 0,
    World,
};

public struct DebugDrawData {
    public u32 draw_count = 0;
    public u32 capacity = 0;
};

public struct DebugAABB {
    public f32x3 position = {};
    public f32x3 size = {};
    public f32x3 color = {};
    public DebugDrawCoord coord = DebugDrawCoord::NDC;
};

public struct DebugRect {
    public f32x3 offset = {};
    public f32x2 extent = {};
    public f32x3 color = {};
    public DebugDrawCoord coord = DebugDrawCoord::NDC;
};

public struct DebugDrawer {
    public DrawIndirectCommand aabb_draw_cmd = {};
    public DebugDrawData aabb_data = {};
    public DebugAABB *aabb_buffer = nullptr;

    public DrawIndirectCommand rect_draw_cmd = {};
    public DebugDrawData rect_data = {};
    public DebugRect *rect_buffer = nullptr;
};

func push_draw(__ref DebugDrawData draw_data) -> u32 {
    let index = com::atomic_add(draw_data.draw_count, 1, com::memory_order_acq_rel);
    if (index < draw_data.capacity) {
        return index;
    }

    return ~0u;
}

public func debug_draw_aabb(__ref DebugDrawer drawer, in DebugAABB v) -> void {
    let index = push_draw(drawer.aabb_data);
    if (index != ~0u) {
        com::atomic_add(drawer.aabb_draw_cmd.instance_count, 1, com::memory_order_acq_rel);
        drawer.aabb_buffer[index] = v;
    }
}

public func debug_draw_rect(__ref DebugDrawer drawer, in DebugRect v) -> void {
    let index = push_draw(drawer.rect_data);
    if (index != ~0u) {
        com::atomic_add(drawer.rect_draw_cmd.instance_count, 1, com::memory_order_acq_rel);
        drawer.rect_buffer[index] = v;
    }
}