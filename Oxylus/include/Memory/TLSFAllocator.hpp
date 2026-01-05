#pragma once

#include "Core/Types.hpp"

namespace ox {
// http://www.gii.upv.es/tlsf/files/papers/ecrts04_tlsf.pdf
struct TLSFAllocator {
  constexpr static usize SL_INDEX_COUNT_LOG2 = 5;
  constexpr static usize SL_INDEX_COUNT = 1 << SL_INDEX_COUNT_LOG2;
  constexpr static usize FL_INDEX_MAX = 30;
  constexpr static usize SMALL_BLOCK_SIZE = 256;

  enum struct NodeID : u32 { Invalid = ~0_u32 };
  struct Node {
    u32 offset = 0;
    u32 size : 31 = 0;
    bool used : 1 = false;
    NodeID prev_phys = NodeID::Invalid;
    NodeID next_phys = NodeID::Invalid;
    NodeID prev_free = NodeID::Invalid;
    NodeID next_free = NodeID::Invalid;
  };

  u32 first_level_bitmap = 0;
  u32 second_level_bitmap[FL_INDEX_MAX] = {};
};
} // namespace ox
