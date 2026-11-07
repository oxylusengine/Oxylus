/**********************************************************************
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/

import common;
import gpu;

// tables from Heitz et al. 2019, "A Low-Discrepancy Sampler that Distributes Monte Carlo Errors
// as a Blue Noise in Screen Space", sobol then scrambling, packed into u32 words
[[vk::binding(0, 0)]] StructuredBuffer<u32> blue_noise_tables;
[[vk::binding(1, 0)]] StorageImage2D<f32x2, ImageFormat::RG8> blue_noise_texture;

static const u32 SOBOL_OFFSET = 0;
static const u32 SCRAMBLING_OFFSET = 65536;
static const f32 GOLDEN_RATIO = 1.61803398875;

func load_table_byte(u32 byte_offset) -> u32 {
    return (blue_noise_tables[byte_offset >> 2] >> ((byte_offset & 3) * 8)) & 0xFF;
}

func sample_random_number(u32 pixel_i, u32 pixel_j, u32 sample_index, u32 sample_dimension) -> f32 {
    let i = pixel_i & 127;
    let j = pixel_j & 127;
    let dimension = sample_dimension & 255;
    let ranked_sample_index = sample_index & 255;

    var value = load_table_byte(SOBOL_OFFSET + dimension + ranked_sample_index * 256);
    value = value ^ load_table_byte(SCRAMBLING_OFFSET + (dimension % 8) + (i + j * 128) * 8);

    return (f32(value) + 0.5) / 256.0;
}

func sample_random_vector_2d(u32x2 pixel, u32 frame_index) -> f32x2 {
    let offset = f32(frame_index & 0xFF) * GOLDEN_RATIO;
    return f32x2(
        fmod(sample_random_number(pixel.x, pixel.y, 0, 0) + offset, 1.0),
        fmod(sample_random_number(pixel.x, pixel.y, 0, 1) + offset, 1.0));
}

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x2 thread_id : SV_DispatchThreadID, uniform u32 frame_index) -> void {
    blue_noise_texture.Store(thread_id, sample_random_vector_2d(thread_id, frame_index));
}
