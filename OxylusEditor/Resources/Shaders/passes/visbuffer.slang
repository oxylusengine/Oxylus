module visbuffer;

import common;
import scene;

constexpr static u64 MESHLET_DEPTH_BITS = 32u;
constexpr static u64 MESHLET_DEPTH_MASK = (1u << MESHLET_DEPTH_BITS) - 1u;

constexpr static u32 MESHLET_INSTANCE_ID_BITS = 24u;
constexpr static u32 MESHLET_INSTANCE_ID_MASK = (1u << MESHLET_INSTANCE_ID_BITS) - 1u;
// We can support up to 256 triangles per meshlet.
// Do not ever go higher than that.
public constexpr static u32 MESHLET_PRIMITIVE_BITS = 8u;
public constexpr static u32 MESHLET_PRIMITIVE_MASK = (1u << MESHLET_PRIMITIVE_BITS) - 1u;

public struct VisBufferData {
    public u32 meshlet_instance_index;
    public u32 triangle_index;

    [[mutating]]
    public __init(u32 meshlet_instance_index, u32 triangle_index) {
        this.meshlet_instance_index = meshlet_instance_index;
        this.triangle_index = triangle_index;
    }

    [[mutating]]
    public __init(u32 data) {
        this.meshlet_instance_index = (data >> MESHLET_PRIMITIVE_BITS) & MESHLET_INSTANCE_ID_MASK;
        this.triangle_index = data & MESHLET_PRIMITIVE_MASK;
    }

    public func encode() -> u32 {
        return (this.meshlet_instance_index << MESHLET_PRIMITIVE_BITS) | (this.triangle_index & MESHLET_PRIMITIVE_MASK);
    }
};

// NOTE: This version of visibility buffering is not widely available.
// My current idea is to have 2 (R32, D32) attachments and render them
// just normally, and then have a compute pass to merge them into R64
// image. R64 images are not widely available even with storage only.
// Investigate maintenance8 for D32<->R32 copies.

public struct VisBuffer {
    u32 data;
    public f32 depth;

    [[mutating]]
    public __init(u32 meshlet_instance_index, u32 triangle_index, f32 depth) {
        const let data = VisBufferData(meshlet_instance_index, triangle_index);
        this.data = data.encode();
        this.depth = depth;
    }

    [[mutating]]
    public __init(in VisBufferData data, f32 depth) {
        this.data = data.encode();
        this.depth = depth;
    }

    [[mutating]]
    public __init(u64 data) {
        this.data = u32(data & MESHLET_DEPTH_MASK);
        this.depth = asfloat(u32(data >> MESHLET_DEPTH_BITS));
    }

    public func encode() -> u64 {
        return (u64(asuint(this.depth)) << MESHLET_DEPTH_BITS) | u64(this.data);
    }

    public func decode_vis() -> VisBufferData {
        return VisBufferData(this.data);
    };
};

