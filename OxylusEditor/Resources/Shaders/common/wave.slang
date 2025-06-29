implementing common;

namespace com {
public func subgroup_id() -> u32 {
    return spirv_asm {
        result:$$u32 = OpLoad builtin(SubgroupId:u32);
    };
}

public func wave_shuffle_xor<T : __BuiltinType>(T value, u32 mask) -> T {
    return spirv_asm {
        OpCapability GroupNonUniformShuffle;
        OpGroupNonUniformShuffleXor $$T result Subgroup $value $mask;
    };
}
}
