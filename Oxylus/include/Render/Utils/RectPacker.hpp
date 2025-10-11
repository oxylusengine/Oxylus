#pragma once

#include <stb_rect_pack.h>
#include <vector>

namespace ox {
class RectPacker {
public:
  using Rect = stbrp_rect;

  // Convenience state wrapper around stb_rect_pack
  //	This can be used to automatically grow backing memory for the packer,
  //	and also supports iteratively finding a minimal containing size when packing
  struct State {
    stbrp_context context = {};
    std::vector<stbrp_node> nodes;
    std::vector<Rect> rects;
    int width = 0;
    int height = 0;

    // Clears the rectangles to empty, but doesn't reset the containing width/height result
    void clear();

    // Add a new rect and also grow the minimum containing size
    void add_rect(const Rect& rect);

    // Performs packing of the rect array
    //	The rectangle offsets will be filled after this
    //	The width, height for minimal containing area will be filled after this if the packing is successful
    //	max_width : if the packing is unsuccessful above this, it will result in a failure
    //	returns true for success, false for failure
    bool pack(int max_width);
  };
};
} // namespace ox
