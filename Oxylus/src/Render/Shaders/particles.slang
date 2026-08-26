module particles;

import common;
import gpu;
import scene;

#include <defines.slang>

public static const u32 PARTICLE_REGISTER_COUNT = 16;
public static const u32 PARTICLE_SIMULATE_GROUP = 64;
public static const u32 PARTICLE_SORT_GROUP = 256;
public static const u32 PARTICLE_CURVE_ATLAS_WIDTH = 64;
public static const u32 PARTICLE_USER_PARAM_COUNT = 4;

public enum ParticleRegister : u32 {
    PositionLife = 0,
    VelocityTotal,
    SizeRotationSeed,
    Color,
    Context,
};

public enum ParticleOpCode : u32 {
    Nop = 0,
    Mov,
    Swizzle,
    Add,
    Sub,
    Mul,
    Div,
    Mad,
    Min,
    Max,
    Clamp,
    Lerp,
    Abs,
    Floor,
    Frac,
    Pow,
    Dot,
    Cross,
    Normalize,
    Length,
    Sin,
    Cos,
    Step,
    Smoothstep,
    Select,
    Curve,
    Gradient,
    Noise,
    Random,
    Time,
    AgeNorm,
    Param,
    // CPU-only, never emitted into a GPU program. Listed so the numbering matches SceneGPU.hpp.
    LoadState,
    StoreState,
};

public enum ParticleOperandKind : u32 {
    Register = 0,
    Constant,
    Immediate,
};

[[Flags]]
public enum ParticleEmitterFlags : u32 {
    LocalSpace,
    Additive,
    VelocityStretched,
    SoftParticles,
    DepthCollision,
    MeshRenderer,
    HorizontalPlane,
    VerticalPlane,
};

public enum ParticleShape : u32 {
    Point = 0,
    Sphere,
    Hemisphere,
    Box,
    Circle,
    Cone,
};

public struct ParticleInstruction {
    public u32 op_dst;
    public u32 src0;
    public u32 src1;
    public u32 src2;
};

public struct Particle {
    public f32x3 position;
    public f32 life_remaining;
    public f32x3 velocity;
    public f32 life_total;
    public f32x2 size;
    public f32 rotation;
    public f32 seed;
    public u32 color;
    public u32 flipbook_frame;
    public u32 emitter_index;
    public u32 flags;
};

public struct ParticleEmitter {
    public mat4 transform;
    public f32x4 rotation;

    public u32 pool_offset;
    public u32 capacity;
    public u32 spawn_count;
    public u32 spawn_offset;

    public u32 spawn_program_offset;
    public u32 spawn_program_count;
    public u32 update_program_offset;
    public u32 update_program_count;

    public u32 constants_offset;
    public u32 curve_atlas_index;
    public u32 curve_sampler_index;
    public u32 curve_row_count;

    public u32 atlas_row_count;
    public u32 material_index;
    public u32 flipbook_x;
    public u32 flipbook_y;

    public u32 flags;
    public u32 shape;
    public u32 seed;
    public f32 time;

    public f32 delta_time;
    public f32 restitution;
    public f32 soft_particle_distance;
    public f32 velocity_stretch;

    public f32x2 lifetime;
    public f32x4 shape_params;
    public f32x4 velocity_offset;
    public f32x4 user_params[PARTICLE_USER_PARAM_COUNT];
};

public struct ParticleSortKey {
    public u32 key;
    public u32 index;
};

public struct ParticleCounters {
    public u32 alive_count;
    public u32 alive_count_next;
    public u32 draw_count;
    public u32 pad;
};

public func particle_pack_rgba8(f32x4 color) -> u32 {
    let c = u32x4(saturate(color) * 255.0 + 0.5);
    return c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
}

public func particle_unpack_rgba8(u32 packed) -> f32x4 {
    return f32x4(u32x4(packed, packed >> 8u, packed >> 16u, packed >> 24u) & 0xFFu) / 255.0;
}

public func particle_hash(u32 value) -> u32 {
    var x = value;
    x ^= x >> 17;
    x *= 0xed5ad4bbu;
    x ^= x >> 11;
    x *= 0xac4c1b51u;
    x ^= x >> 15;
    x *= 0x31848babu;
    x ^= x >> 14;
    return x;
}

public func particle_random(u32 value) -> f32 {
    return f32(particle_hash(value) & 0x00FFFFFFu) / f32(0x01000000u);
}

public func particle_random3(u32 value) -> f32x3 {
    return f32x3(particle_random(value), particle_random(value + 0x9E3779B9u), particle_random(value + 0x85EBCA6Bu));
}

func particle_value_noise(f32x3 p) -> f32 {
    let i = floor(p);
    let f = frac(p);
    let u = f * f * (3.0 - 2.0 * f);

    var result = 0.0;
    for (u32 corner = 0; corner < 8; corner++) {
        let offset = f32x3(f32(corner & 1u), f32((corner >> 1) & 1u), f32((corner >> 2) & 1u));
        let cell = i + offset;
        let hash = particle_hash(u32(i32(cell.x)) * 73856093u ^ u32(i32(cell.y)) * 19349663u ^ u32(i32(cell.z)) * 83492791u);
        let value = f32(hash & 0x00FFFFFFu) / f32(0x01000000u) * 2.0 - 1.0;
        let weight = lerp(1.0 - u.x, u.x, offset.x) * lerp(1.0 - u.y, u.y, offset.y) * lerp(1.0 - u.z, u.z, offset.z);
        result += value * weight;
    }

    return result;
}

func particle_noise_potential(f32x3 p) -> f32x3 {
    return f32x3(
        particle_value_noise(p),
        particle_value_noise(p + f32x3(31.416, 47.853, 12.793)),
        particle_value_noise(p + f32x3(-17.239, 63.121, 91.007))
    );
}

public func particle_curl_noise(f32x3 p) -> f32x3 {
    const f32 e = 0.1;

    let dx = f32x3(e, 0.0, 0.0);
    let dy = f32x3(0.0, e, 0.0);
    let dz = f32x3(0.0, 0.0, e);

    let px0 = particle_noise_potential(p - dx);
    let px1 = particle_noise_potential(p + dx);
    let py0 = particle_noise_potential(p - dy);
    let py1 = particle_noise_potential(p + dy);
    let pz0 = particle_noise_potential(p - dz);
    let pz1 = particle_noise_potential(p + dz);

    let x = (py1.z - py0.z) - (pz1.y - pz0.y);
    let y = (pz1.x - pz0.x) - (px1.z - px0.z);
    let z = (px1.y - px0.y) - (py1.x - py0.x);

    return f32x3(x, y, z) / (2.0 * e);
}

public func particle_rotate_by_quat(f32x4 q, f32x3 v) -> f32x3 {
    let t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

public func particle_sample_atlas(in ParticleEmitter emitter, u32 row, f32 t) -> f32x4 {
    if (emitter.curve_atlas_index == ~0u) {
        return f32x4(1.0);
    }

    let uv = f32x2(saturate(t), (f32(row) + 0.5) / f32(max(emitter.atlas_row_count, 1u)));

    return global_images[NonUniformResourceIndex(emitter.curve_atlas_index)]
        .SampleLevel(global_samplers[NonUniformResourceIndex(emitter.curve_sampler_index)], uv, 0.0);
}

public struct ParticleVM {
    public f32x4 regs[PARTICLE_REGISTER_COUNT];
    public u32 rng;

    public __init(in Particle particle, in ParticleEmitter emitter, f32 age_norm) {
        for (u32 i = 0; i < PARTICLE_REGISTER_COUNT; i++) {
            this.regs[i] = f32x4(0.0);
        }

        this.regs[ParticleRegister.PositionLife] = f32x4(particle.position, particle.life_remaining);
        this.regs[ParticleRegister.VelocityTotal] = f32x4(particle.velocity, particle.life_total);
        this.regs[ParticleRegister.SizeRotationSeed] = f32x4(particle.size, particle.rotation, particle.seed);
        this.regs[ParticleRegister.Color] = particle_unpack_rgba8(particle.color);
        this.regs[ParticleRegister.Context] = f32x4(age_norm, emitter.time, emitter.delta_time, f32(particle.flipbook_frame));
        this.rng = particle_hash(asuint(particle.seed));
    }

    public func store(inout Particle particle) {
        particle.position = this.regs[ParticleRegister.PositionLife].xyz;
        particle.life_remaining = this.regs[ParticleRegister.PositionLife].w;
        particle.velocity = this.regs[ParticleRegister.VelocityTotal].xyz;
        particle.life_total = this.regs[ParticleRegister.VelocityTotal].w;
        particle.size = this.regs[ParticleRegister.SizeRotationSeed].xy;
        particle.rotation = this.regs[ParticleRegister.SizeRotationSeed].z;
        particle.seed = this.regs[ParticleRegister.SizeRotationSeed].w;
        particle.color = particle_pack_rgba8(this.regs[ParticleRegister.Color]);
    }
}

func particle_fetch(
    in ParticleVM vm,
    in ParticleEmitter emitter,
    StructuredBuffer<f32x4> constants,
    u32 operand
) -> f32x4 {
    let kind = ParticleOperandKind(operand >> 30u);
    let payload = operand & 0x3FFFFFFFu;

    if (kind == ParticleOperandKind.Register) {
        return vm.regs[payload & (PARTICLE_REGISTER_COUNT - 1u)];
    }

    if (kind == ParticleOperandKind.Constant) {
        return constants[emitter.constants_offset + payload];
    }

    return f32x4(f32(payload));
}

func particle_swizzle(f32x4 v, u32 mask) -> f32x4 {
    return f32x4(v[mask & 3u], v[(mask >> 2u) & 3u], v[(mask >> 4u) & 3u], v[(mask >> 6u) & 3u]);
}

public func particle_execute(
    inout ParticleVM vm,
    in ParticleEmitter emitter,
    StructuredBuffer<ParticleInstruction> program,
    StructuredBuffer<f32x4> constants,
    u32 offset,
    u32 count
) {
    for (u32 pc = 0; pc < count; pc++) {
        let instruction = program[offset + pc];
        let opcode = ParticleOpCode(instruction.op_dst & 0xFFu);
        let dst = (instruction.op_dst >> 8u) & 0xFu;
        let write_mask = (instruction.op_dst >> 12u) & 0xFu;

        let a = particle_fetch(vm, emitter, constants, instruction.src0);
        let b = particle_fetch(vm, emitter, constants, instruction.src1);
        let c = particle_fetch(vm, emitter, constants, instruction.src2);

        var result = f32x4(0.0);
        switch (opcode) {
            case ParticleOpCode.Mov       : result = a; break;
            case ParticleOpCode.Swizzle   : result = particle_swizzle(a, instruction.src1 & 0x3FFFFFFFu); break;
            case ParticleOpCode.Add       : result = a + b; break;
            case ParticleOpCode.Sub       : result = a - b; break;
            case ParticleOpCode.Mul       : result = a * b; break;
            case ParticleOpCode.Div       : result = a / max(abs(b), f32x4(1e-6)) * sign(b); break;
            case ParticleOpCode.Mad       : result = a * b + c; break;
            case ParticleOpCode.Min       : result = min(a, b); break;
            case ParticleOpCode.Max       : result = max(a, b); break;
            case ParticleOpCode.Clamp     : result = clamp(a, b, c); break;
            case ParticleOpCode.Lerp      : result = lerp(a, b, c); break;
            case ParticleOpCode.Abs       : result = abs(a); break;
            case ParticleOpCode.Floor     : result = floor(a); break;
            case ParticleOpCode.Frac      : result = frac(a); break;
            case ParticleOpCode.Pow       : result = pow(max(a, f32x4(0.0)), b); break;
            case ParticleOpCode.Dot       : result = f32x4(dot(a.xyz, b.xyz)); break;
            case ParticleOpCode.Cross     : result = f32x4(cross(a.xyz, b.xyz), 0.0); break;
            case ParticleOpCode.Normalize : result = f32x4(normalize(a.xyz + f32x3(1e-9)), 0.0); break;
            case ParticleOpCode.Length    : result = f32x4(length(a.xyz)); break;
            case ParticleOpCode.Sin       : result = sin(a); break;
            case ParticleOpCode.Cos       : result = cos(a); break;
            case ParticleOpCode.Step      : result = step(a, b); break;
            case ParticleOpCode.Smoothstep: result = smoothstep(a, b, c); break;
            case ParticleOpCode.Select    : result = lerp(a, b, step(f32x4(0.5), c)); break;
            case ParticleOpCode.Curve     : result = particle_sample_atlas(emitter, instruction.src1 & 0x3FFFFFFFu, a.x); break;
            case ParticleOpCode.Gradient  :
                result = particle_sample_atlas(emitter, emitter.curve_row_count + (instruction.src1 & 0x3FFFFFFFu), a.x);
                break;
            case ParticleOpCode.Noise     : result = f32x4(particle_curl_noise(a.xyz * max(b.x, 1e-4)), 0.0); break;
            case ParticleOpCode.Random    : {
                let stream = instruction.src2 & 0x3FFFFFFFu;
                let base = vm.rng + stream * 0x9E3779B9u;
                let r = f32x4(
                    particle_random(base),
                    particle_random(base + 1u),
                    particle_random(base + 2u),
                    particle_random(base + 3u)
                );
                result = lerp(a, b, r);
            } break;
            case ParticleOpCode.Time      : result = f32x4(vm.regs[ParticleRegister.Context].y); break;
            case ParticleOpCode.AgeNorm  : result = f32x4(vm.regs[ParticleRegister.Context].x); break;
            case ParticleOpCode.Param    :
                result = emitter.user_params[(instruction.src1 & 0x3FFFFFFFu) % PARTICLE_USER_PARAM_COUNT];
                break;
            default                    : break;
        }

        let target = dst & (PARTICLE_REGISTER_COUNT - 1u);
        var current = vm.regs[target];
        if ((write_mask & 1u) != 0u) { current.x = result.x; }
        if ((write_mask & 2u) != 0u) { current.y = result.y; }
        if ((write_mask & 4u) != 0u) { current.z = result.z; }
        if ((write_mask & 8u) != 0u) { current.w = result.w; }
        vm.regs[target] = current;
    }
}

public func particle_emission_point(in ParticleEmitter emitter, u32 rng, out f32x3 direction) -> f32x3 {
    let r = particle_random3(rng);
    let r2 = particle_random3(rng + 0x68bc21ebu);

    var local = f32x3(0.0);
    direction = f32x3(0.0, 1.0, 0.0);

    switch (ParticleShape(emitter.shape)) {
        case ParticleShape.Sphere: {
            let theta = r.x * TAU;
            let z = r.y * 2.0 - 1.0;
            let radius = sqrt(max(1.0 - z * z, 0.0));
            direction = f32x3(radius * cos(theta), z, radius * sin(theta));
            local = direction * emitter.shape_params.xyz * pow(max(r.z, 1e-6), 1.0 / 3.0);
        } break;
        case ParticleShape.Hemisphere: {
            let theta = r.x * TAU;
            let z = r.y;
            let radius = sqrt(max(1.0 - z * z, 0.0));
            direction = f32x3(radius * cos(theta), z, radius * sin(theta));
            local = direction * emitter.shape_params.xyz * pow(max(r.z, 1e-6), 1.0 / 3.0);
        } break;
        case ParticleShape.Box: {
            local = (r * 2.0 - 1.0) * emitter.shape_params.xyz;
            direction = f32x3(0.0, 1.0, 0.0);
        } break;
        case ParticleShape.Circle: {
            let theta = r.x * TAU;
            let radius = sqrt(max(r.y, 0.0));
            direction = f32x3(cos(theta), 0.0, sin(theta));
            local = direction * emitter.shape_params.xyz * radius;
        } break;
        case ParticleShape.Cone: {
            let theta = r.x * TAU;
            // normalized radius, so the spread is the authored angle no matter how big the cone is
            let unit_radius = sqrt(max(r.y, 0.0));
            let spread = tan(radians(clamp(emitter.shape_params.w, 0.0, 89.0)));
            let ring = f32x3(cos(theta), 0.0, sin(theta));
            let height = emitter.shape_params.y * r.z;
            local = ring * (emitter.shape_params.x + height * spread) * unit_radius + f32x3(0.0, height, 0.0);
            direction = normalize(f32x3(0.0, 1.0, 0.0) + ring * spread * unit_radius);
        } break;
        default: {
            local = f32x3(0.0);
            direction = normalize(r2 * 2.0 - 1.0 + f32x3(1e-6));
        } break;
    }

    return local;
}
