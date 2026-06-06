module common;

__include common.stdint;

__include common.color;
__include common.encoding;
__include common.math;
__include common.wave;

public constexpr func HAS_FLAG<T: __EnumType>(constexpr T mask, constexpr T bit) -> bool {
    return (mask & bit) == true;
}