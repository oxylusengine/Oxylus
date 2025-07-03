implementing common;

public namespace com {
// Can support up to SPIR-V 1.5
public enum MemoryOrder : u32 {
    Relaxed = 0x0,
    Acquire = 0x2,
    Release = 0x4,
    AcqRel  = 0x8,
    SeqCst  = 0x10,
};
public static constexpr MemoryOrder memory_order_relaxed = MemoryOrder::Relaxed;
public static constexpr MemoryOrder memory_order_acquire = MemoryOrder::Acquire;
public static constexpr MemoryOrder memory_order_release = MemoryOrder::Release;
public static constexpr MemoryOrder memory_order_acq_rel = MemoryOrder::AcqRel;
public static constexpr MemoryOrder memory_order_seq_cst = MemoryOrder::SeqCst;

public enum MemoryLocation : u32 {
    None      = 0,
    Buffer    = 0x40,
    Subgroup  = 0x80,
    Workgroup = 0x100,
    Image     = 0x800,
};

func spirv_type_checks<T>() -> void {
    if (__type_equals<T, f32>()) {
        spirv_asm {
            OpExtension "SPV_EXT_shader_atomic_float_add";
            OpCapability AtomicFloat32MinMaxEXT
        };
    } else if (__type_equals<T, f16>()) {
        spirv_asm {
            OpExtension "SPV_EXT_shader_atomic_float_add";
            OpCapability AtomicFloat16MinMaxEXT
        };
    } else if (__type_equals<T, f64>()) {
        spirv_asm {
            OpExtension "SPV_EXT_shader_atomic_float_add";
            OpCapability AtomicFloat64MinMaxEXT
        };
    } else if (__type_equals<T, u64>() ||
               __type_equals<T, i64>()) {
        spirv_asm {
            OpCapability Int64Atomics
        };
    }
}

[[ForceInline]]
public func atomic_load<T>(
    __ref T dst,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicLoad &dst $scope $semantics;
    };
}

[[ForceInline]]
public func atomic_store<T>(
    __ref T dst,
    T desired,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> void {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    spirv_asm {
        OpAtomicStore &dst $scope $semantics $desired;
    };
}

[[ForceInline]]
public func atomic_increment<T : IInteger>(
    __ref T dst,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicIIncrement &dst $scope $semantics;
    };
}

[[ForceInline]]
public func atomic_decrement<T : IInteger>(
    __ref T dst,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicIDecrement &dst $scope $semantics;
    };
}

[[ForceInline]]
public func atomic_add<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    constexpr u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicIAdd &dst $scope $semantics $value;
    };
}

[[ForceInline]]
public func atomic_sub<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    constexpr u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicISub &dst $scope $semantics $value;
    };
}

[[ForceInline]]
public func atomic_max<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    if (__isUnsignedInt<T>()) {
        return spirv_asm {
            result:$$T = OpAtomicUMax &dst $scope $semantics $value;
        };
    } else if (__isSignedInt<T>()) {
        return spirv_asm {
            result:$$T = OpAtomicSMax &dst $scope $semantics $value;
        };
    } else {
        spirv_asm { "<invalid atomic_min>" };
    }

    return {};
}

[[ForceInline]]
public func atomic_min<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    if (__isUnsignedInt<T>()) {
        return spirv_asm {
            result:$$T = OpAtomicUMin &dst $scope $semantics $value;
        };
    } else if (__isSignedInt<T>()) {
        return spirv_asm {
            result:$$T = OpAtomicSMin &dst $scope $semantics $value;
        };
    } else {
        spirv_asm { "<invalid atomic_min>" };
    }

    return {};
}

[[ForceInline]]
public func atomic_and<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicAnd &dst $scope $semantics $value;
    };
}

[[ForceInline]]
public func atomic_or<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicOr &dst $scope $semantics $value;
    };
}

[[ForceInline]]
public func atomic_xor<T : IInteger>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicXor &dst $scope $semantics $value;
    };
}

// Floating point atomic extension ──────────────────────────────────
[[ForceInline]]
public func atomic_add<T : IFloat>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicFAddEXT &dst $scope $semantics $value;
    };
}

[[ForceInline]]
public func atomic_max<T : IFloat>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicFMaxEXT &dst $scope $semantics $value;
    };
}

[[ForceInline]]
public func atomic_min<T : IFloat>(
    __ref T dst,
    T value,
    constexpr MemoryOrder memory_order,
    constexpr MemoryLocation location = MemoryLocation::None,
    constexpr MemoryScope scope = MemoryScope::Workgroup
) -> T {
    spirv_type_checks<T>();
    const u32 semantics = memory_order | location;
    return spirv_asm {
        result:$$T = OpAtomicFMinEXT &dst $scope $semantics $value;
    };
}

// Base type for atomics. Can perform very basic operations
// where it's supported without extensions.
public struct atomic<T> {
    T value;

    [[ForceInline]]
    [[mutating]]
    public __init(T v) {
        this.store(v, com::memory_order_acq_rel);
    }

    [[ForceInline]]
    [[mutating]]
    public func load(
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_load(this.value, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func store(
        T desired,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> void {
        atomic_store(this.value, desired, memory_order, location, scope);
    }
};

// Integer extension ────────────────────────────────────────────────
public extension<T : IInteger> atomic<T> {
    [[ForceInline]]
    [[mutating]]
    public func fetch_add(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_add(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_sub(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_sub(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_max(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_max(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_min(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_min(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_and(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_and(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_or(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_or(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_xor(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_xor(this.value, arg, memory_order, location, scope);
    }
};

// Floating point extension ─────────────────────────────────────────
public extension<T : IFloat> atomic<T> {
    [[ForceInline]]
    [[mutating]]
    public func fetch_add(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_add(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_max(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_max(this.value, arg, memory_order, location, scope);
    }

    [[ForceInline]]
    [[mutating]]
    public func fetch_min(
        T arg,
        constexpr MemoryOrder memory_order,
        constexpr MemoryLocation location = MemoryLocation::None,
        constexpr MemoryScope scope = MemoryScope::Workgroup
    ) -> T {
        return atomic_min(this.value, arg, memory_order, location, scope);
    }
};

// Barriers ─────────────────────────────────────────────────────────

// Wait for all invocations in the scope restricted tangle to reach
// the current point of execution before executing further instructions.
//
// Execution is the scope defining the scope restricted tangle
// affected by this command.
//
// https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpControlBarrier
[[ForceInline]]
public func control_barrier(
    constexpr MemoryOrder memory_order,
    constexpr MemoryScope scope_execution = MemoryScope::Workgroup,
    constexpr MemoryScope scope = MemoryScope::Workgroup,
    constexpr MemoryLocation location = MemoryLocation::Workgroup
) -> void {
    constexpr u32 semantics = memory_order | location;
    spirv_asm {
        OpControlBarrier $scope_execution $scope $semantics;
    };
}

// Good old barrier.
[[ForceInline]]
public func memory_barrier(
    constexpr MemoryOrder memory_order,
    constexpr MemoryScope scope = MemoryScope::Workgroup,
    constexpr MemoryLocation location = MemoryLocation::None
) -> void {
    constexpr u32 semantics = memory_order | location;
    spirv_asm {
        OpMemoryBarrier $scope $semantics;
    };
}
}
