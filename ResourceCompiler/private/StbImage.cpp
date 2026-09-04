// The compiler needs stb's decoder and resampler, and Oxylus defines both in `Asset/Texture.cpp`.
// Referencing them there would make the linker pull that whole object -- and with it vuk,
// RenderContext, App and ImGui -- into this shared library, giving the process a second copy of
// every engine global the editor already owns. So the implementation lives here instead.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include <stb_image.h>
#include <stb_image_resize2.h>
