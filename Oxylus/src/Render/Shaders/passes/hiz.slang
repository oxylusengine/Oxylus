import common;
import gpu;

// Credits:
// - https://github.dev/SparkyPotato/radiance/blob/main/shaders/passes/mesh/hzb.slang
// - https://github.com/Themaister/Granite/blob/master/assets/shaders/post/hiz.comp
// - AMD's Single Pass Downsampler.

[[vk::binding(0)]] globallycoherent RWStructuredBuffer<u32> spd_global_atomic;

[[vk::binding(1)]] Sampler sampler;
[[vk::binding(2)]] Image2D<f32> src_image;

typealias HiZMip = StorageImage2D<f32, ImageFormat::R32F>;
[[vk::binding(3)]]  HiZMip dst_mip_0;
[[vk::binding(4)]]  HiZMip dst_mip_1;
[[vk::binding(5)]]  HiZMip dst_mip_2;
[[vk::binding(6)]]  HiZMip dst_mip_3;
[[vk::binding(7)]]  HiZMip dst_mip_4;
[[vk::binding(8)]]  HiZMip dst_mip_5;
[[vk::binding(9)]]  globallycoherent  HiZMip dst_mip_6;
[[vk::binding(10)]] HiZMip dst_mip_7;
[[vk::binding(11)]] HiZMip dst_mip_8;
[[vk::binding(12)]] HiZMip dst_mip_9;
[[vk::binding(13)]] HiZMip dst_mip_10;
[[vk::binding(14)]] HiZMip dst_mip_11;
[[vk::binding(15)]] HiZMip dst_mip_12;

[[vk::constant_id(0)]] constexpr u32 is_pot = 0;
[[vk::constant_id(1)]] constexpr u32 src_width = 0;
[[vk::constant_id(2)]] constexpr u32 src_height = 0;
constexpr static u32x2 src_extent = u32x2(src_width, src_height);
constexpr static f32x2 inv_src_extent = 1.0 / f32x2(src_extent);

struct PushConstants {
    u32 mip_count;
    u32 work_group_count;
    f32x2x2 transform_z;
};
[[vk::push_constant]] PushConstants C;

func store(u32x2 texel, u32 mip, f32 v) -> void {
    switch(mip) {
        case 0: dst_mip_0.Store(texel, v); return;
        case 1: dst_mip_1.Store(texel, v); return;
        case 2: dst_mip_2.Store(texel, v); return;
        case 3: dst_mip_3.Store(texel, v); return;
        case 4: dst_mip_4.Store(texel, v); return;
        case 5: dst_mip_5.Store(texel, v); return;
        case 6: dst_mip_6.Store(texel, v); return;
        case 7: dst_mip_7.Store(texel, v); return;
        case 8: dst_mip_8.Store(texel, v); return;
        case 9: dst_mip_9.Store(texel, v); return;
        case 10: dst_mip_10.Store(texel, v); return;
        case 11: dst_mip_11.Store(texel, v); return;
        default: dst_mip_12.Store(texel, v); return;
    }
}

func unswizzle_16x16(u32 i) -> u32x2 {
    let x0 = bitfieldExtract(i, 0, 1);
    let y01 = bitfieldExtract(i, 1, 2);
    let x12 = bitfieldExtract(i, 3, 2);
    let y23 = bitfieldExtract(i, 5, 2);
    let x3 = bitfieldExtract(i, 7, 1);
    return u32x2(bitfieldInsert(bitfieldInsert(x0, x12, 1, 2), x3, 3, 1), bitfieldInsert(y01, y23, 2, 2));
}

func transform_z(f32x4 zs) -> f32x4 {
    let z0 = mul(C.transform_z, f32x2(zs.x, 1.0));
    let z1 = mul(C.transform_z, f32x2(zs.y, 1.0));
    let z2 = mul(C.transform_z, f32x2(zs.z, 1.0));
    let z3 = mul(C.transform_z, f32x2(zs.w, 1.0));
    return f32x4(z0.x, z1.x, z2.x, z3.x) / f32x4(z0.y, z1.y, z2.y, z3.y);
}

func reduce(f32 x, f32 y) -> f32 {
    return min(x, y);
}

func reduce(f32x4 v) -> f32 {
    return reduce(reduce(v.x, v.y), reduce(v.z, v.w));
}

func store_2x2(u32x2 p, u32 mip, f32x4 v) -> void {
    store(p + u32x2(0, 0), mip, v.x);
    store(p + u32x2(1, 0), mip, v.y);
    store(p + u32x2(0, 1), mip, v.z);
    store(p + u32x2(1, 1), mip, v.w);
}

func load(u32x2 texel) -> f32 {
    let uv = f32x2(texel) * inv_src_extent + inv_src_extent;
    return src_image.SampleLevel(sampler, uv, 0);
}

func load_2x2(u32x2 p) -> f32x4 {
    let x = load(p + u32x2(0, 0));
    let y = load(p + u32x2(1, 0));
    let z = load(p + u32x2(0, 1));
    let w = load(p + u32x2(1, 1));
    return f32x4(x, y, z, w);
}

func load_4x4(u32x2 p) -> f32x4x4 {
    let x = load_2x2(p + u32x2(0, 0));
    let y = load_2x2(p + u32x2(2, 0));
    let z = load_2x2(p + u32x2(0, 2));
    let w = load_2x2(p + u32x2(2, 2));
    return f32x4x4(x, y, z, w);
}

func load_mid(u32x2 texel) -> f32 {
    if (is_pot == 0) {
        texel = min(texel, src_extent >> 6);
    }

    return dst_mip_6.Load(texel);
}

func load_mid_2x2(u32x2 p) -> f32x4 {
    let x = load_mid(p + u32x2(0, 0));
    let y = load_mid(p + u32x2(1, 0));
    let z = load_mid(p + u32x2(0, 1));
    let w = load_mid(p + u32x2(1, 1));
    return f32x4(x, y, z, w);
}

func load_mid_4x4(u32x2 p) -> f32x4x4 {
    let x = load_mid_2x2(p + u32x2(0, 0));
    let y = load_mid_2x2(p + u32x2(2, 0));
    let z = load_mid_2x2(p + u32x2(0, 2));
    let w = load_mid_2x2(p + u32x2(2, 2));
    return f32x4x4(x, y, z, w);
}

func reduce_mip(f32x4x4 v, u32x2 p, u32 mip) -> f32 {
    let d0 = reduce(v[0]);
    let d1 = reduce(v[1]);
    let d2 = reduce(v[2]);
    let d3 = reduce(v[3]);
    let ret = f32x4(d0, d1, d2, d3);
    store_2x2(p, mip, ret);
    return reduce(ret);
}

func reduce_mip_simd(u32x2 p, u32 local_id, u32 mip, f32 d) -> f32 {
    let quad = WaveGetLaneIndex() & (~0x3);
    var horiz = WaveReadLaneAt(d, quad | 1);
    var vert = WaveReadLaneAt(d, quad | 2);
    var diag = WaveReadLaneAt(d, quad | 3);
    store(p, mip, d);

    if (C.mip_count > mip + 1) {
        p >>= 1;
        d = reduce(f32x4(d, horiz, vert, diag));
        horiz = com::wave_shuffle_xor(d, 0b1000);
        vert = com::wave_shuffle_xor(d, 0b0100);
        diag = com::wave_shuffle_xor(d, 0b1100);
        if ((local_id & 3) == 0) {
            store(p, mip + 1, d);
        }
    }

    return reduce(f32x4(d, horiz, vert, diag));
}

groupshared f32 shared_buffer[256 / 16];
groupshared bool is_last;

[[shader("compute")]]
[[numthreads(256, 1, 1)]]
func cs_main(u32x2 group_id : SV_GroupID) -> void {
    let local_id = com::subgroup_id() * WaveGetLaneCount() + WaveGetLaneIndex();
    let p = unswizzle_16x16(local_id);
    var base_coord = group_id * 64 + p * 4;

    // MIP 0
    var m = load_4x4(base_coord);
    let texel_00 = transform_z(m[0]);
    let texel_10 = transform_z(m[1]);
    let texel_01 = transform_z(m[2]);
    let texel_11 = transform_z(m[3]);
    m = f32x4x4(texel_00, texel_10, texel_01, texel_11);

    store_2x2(base_coord + u32x2(0, 0), 0, texel_00);
    store_2x2(base_coord + u32x2(2, 0), 0, texel_10);
    store_2x2(base_coord + u32x2(0, 2), 0, texel_01);
    store_2x2(base_coord + u32x2(2, 2), 0, texel_11);
    if (C.mip_count <= 1) {
        return;
    }

    // MIP 1
    var d = reduce_mip(m, base_coord >> 1, 1);
    if (C.mip_count <= 2) {
        return;
    }

    // MIP 2, 3
    d = reduce_mip_simd(base_coord >> 2, local_id, 2, d);
    if (C.mip_count <= 4) {
        return;
    }

    // MIP 4, 5, 6
    if ((local_id & 15) == 0) {
        shared_buffer[local_id >> 4] = d;
    }

    GroupMemoryBarrierWithGroupSync();

    if (local_id < 16) {
        d = reduce_mip_simd(group_id * 4 + p, local_id, 4, shared_buffer[local_id]);
    }

    if (local_id == 0) {
        store(group_id, 6, d);
    }

    if (C.mip_count <= 6) {
        return;
    }

    AllMemoryBarrierWithGroupSync();

    if (local_id == 0) {
        is_last = __atomic_add(spd_global_atomic[0], 1, MemoryOrder::AcquireRelease) == C.work_group_count - 1;
    }

    GroupMemoryBarrierWithGroupSync();
    if (!is_last) {
        return;
    }

    // MIP 7
    base_coord = p * 4;
    d = reduce_mip(load_mid_4x4(base_coord), base_coord >> 1, 7);
    if (C.mip_count <= 8) {
        return;
    }

    // MIP 8, 9
    d = reduce_mip_simd(p, local_id, 8, d);
    if (C.mip_count <= 10) {
        return;
    }

    // MIP 10, 11, 12
    if ((local_id & 15) == 0) {
        shared_buffer[local_id >> 4] = d;
    }

    GroupMemoryBarrierWithGroupSync();

    if (local_id < 16) {
        d = reduce_mip_simd(p, local_id, 10, shared_buffer[local_id]);
    }

    if (C.mip_count <= 12) {
        return;
    }

    if (local_id == 0) {
        store(u32x2(0, 0), 12, d);
    }
}
