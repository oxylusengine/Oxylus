implementing gpu;

import common;

public struct DispatchIndirectCommand {
    public u32 x;
    public u32 y;
    public u32 z;
};

public struct DrawIndexedIndirectCommand {
    public u32 index_count;
    public u32 instance_count;
    public u32 first_index;
    public i32 vertex_offset;
    public u32 first_instance;
};

public enum ImageFormat : i32 {
    Undefined = 0,
    RGBA32F,           // 4 channel 32-bit floating point texture
    RGBA16F,           // 4 channel 16-bit floating point texture
    RG32F,             // 2 channel 32-bit floating point texture
    RG16F,             // 2 channel 16-bit floating point texture
    R11F_G11F_B10F,    // 3 channel 11/11/10-bit floating point texture
    R32F,              // 1 channel 32-bit floating point texture
    R16F,              // 1 channel 16-bit floating point texture
    RGBA16,            // 4 channel 16-bit normalized unsigned integer texture
    RGB10_A2,          // 4 channel 10/10/10/2-bit signed integer texture
    RGBA8,             // 4 channel 8-bit normalized unsigned integer texture
    RG16,              // 2 channel 16-bit normalized unsigned integer texture
    RG8,               // 2 channel 8-bit normalized unsigned integer texture
    R16,               // 1 channel 16-bit normalized unsigned integer texture
    R8,                // 1 channel 8-bit normalized unsigned integer texture
    RGBA16_SNORM,      // 4 channel 16-bit normalized signed integer texture
    RGBA8_SNORM,       // 4 channel 8-bit normalized signed integer texture
    RG16_SNORM,        // 2 channel 16-bit normalized signed integer texture
    RG8_SNORM,         // 2 channel 8-bit normalized signed integer texture
    R16_SNORM,         // 1 channel 16-bit normalized signed integer texture
    R8_SNORM,          // 1 channel 8-bit normalized signed integer texture
    RGBA32I,           // 4 channel 32-bit signed integer texture
    RGBA16I,           // 4 channel 16-bit signed integer texture
    RGBA8I,            // 4 channel 8-bit signed integer texture
    RG32I,             // 2 channel 32-bit signed integer texture
    RG16I,             // 2 channel 16-bit signed integer texture
    RG8I,              // 2 channel 8-bit signed integer texture
    R32I,              // 1 channel 32-bit signed integer texture
    R16I,              // 1 channel 16-bit signed integer texture
    R8I,               // 1 channel 8-bit signed integer texture
    RGBA32UI,          // 4 channel 32-bit unsigned integer texture
    RGBA16UI,          // 4 channel 16-bit unsigned integer texture
    RGB10_A2UI,        // 4 channel 10/10/10/2-bit unsigned integer texture
    RGBA8UI,           // 4 channel 8-bit unsigned integer texture
    RG32UI,            // 2 channel 32-bit unsigned integer texture
    RG16UI,            // 2 channel 16-bit unsigned integer texture
    RG8UI,             // 2 channel 8-bit unsigned integer texture
    R32UI,             // 1 channel 32-bit unsigned integer texture
    R16UI,             // 1 channel 16-bit unsigned integer texture
    R8UI,              // 1 channel 8-bit unsigned integer texture
    R64UI,             // 1 channel 64-bit unsigned integer texture
    R64I               // 1 channel 64-bit signed integer texture
};