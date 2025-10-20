import common;

#include <defines.slang>

constexpr static f32 LUMINANCE_EPSILON = 0.001;

struct HistogramLuminance {
    f32 adapted_luminance;
    f32 exposure;
};
