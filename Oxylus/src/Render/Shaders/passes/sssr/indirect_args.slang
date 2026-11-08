// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

import common;
import gpu;

[[vk::binding(0, 0)]] RWStructuredBuffer<u32> ray_counters;
[[vk::binding(1, 0)]] RWStructuredBuffer<u32> indirect_args;

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main() -> void {
    { // Prepare intersection args
        let ray_count = ray_counters[0];

        indirect_args[0] = (ray_count + 63) / 64;
        indirect_args[1] = 1;
        indirect_args[2] = 1;

        ray_counters[0] = 0;
        ray_counters[1] = ray_count;
    }

    { // Prepare denoiser args
        let tile_count = ray_counters[2];

        indirect_args[3] = tile_count;
        indirect_args[4] = 1;
        indirect_args[5] = 1;

        ray_counters[2] = 0;
        ray_counters[3] = tile_count;
    }
}
