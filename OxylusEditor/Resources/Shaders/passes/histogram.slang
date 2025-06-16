module histogram;

import common;

constexpr static f32 LUMINANCE_EPSILON = 0.001;

#ifndef HISTOGRAM_THREADS_X
#define HISTOGRAM_THREADS_X 16
#endif

#ifndef HISTOGRAM_THREADS_Y
#define HISTOGRAM_THREADS_Y 16
#endif

#define HISTOGRAM_BIN_COUNT (HISTOGRAM_THREADS_X * HISTOGRAM_THREADS_Y * 1)

struct HistogramLuminance {
    f32 adapted_luminance;
    f32 exposure;
};
