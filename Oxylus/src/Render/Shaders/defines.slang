#ifndef CULLING_MESH_COUNT
#define CULLING_MESH_COUNT 64
#endif

#ifndef CULLING_MESHLET_COUNT
#define CULLING_MESHLET_COUNT 64
#endif

#ifndef CULLING_TRIANGLE_COUNT
#define CULLING_TRIANGLE_COUNT 64
#endif

#ifndef MESH_MAX_LODS
#define MESH_MAX_LODS 8
#endif

#ifndef MAX_DIRECTIONAL_SHADOW_CASCADES
#define MAX_DIRECTIONAL_SHADOW_CASCADES 4
#endif

#ifndef HISTOGRAM_THREADS_X
#define HISTOGRAM_THREADS_X 16
#endif

#ifndef HISTOGRAM_THREADS_Y
#define HISTOGRAM_THREADS_Y 16
#endif

// Workgroup Y size for `rmvsm_invalidate_pages`. Must match dispatch in Shadowmaps.cpp.
#ifndef INVALIDATE_PAGES_WORKGROUP_Y
#define INVALIDATE_PAGES_WORKGROUP_Y 64
#endif

#define HISTOGRAM_BIN_COUNT (HISTOGRAM_THREADS_X * HISTOGRAM_THREADS_Y * 1)

#define HAS_FLAG(mask, bit) ((mask & bit) != 0)