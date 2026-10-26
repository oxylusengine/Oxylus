#pragma once

#include <expected>
#include <vulkan/vulkan_core.h>

#include "Render/Fwd.hpp"

namespace ox {
struct PendingPick {
  BufferID buffer = BufferID::Invalid;
  vir::SubmitTicket ticket = {};
  bool pending = false;

  auto request(this PendingPick& self) -> bool {
    if (self.pending) {
      return false;
    }

    self.ticket = {};
    self.pending = true;
    return true;
  }

  auto submitted(this PendingPick& self, const std::expected<vir::SubmitTicket, VkResult>& result) -> void {
    if (!self.pending || self.ticket.queue != vir::QueueID::Invalid) {
      return;
    }

    if (!result || result->queue == vir::QueueID::Invalid) {
      self.pending = false;
      return;
    }

    self.ticket = *result;
  }

  auto is_ready(this const PendingPick& self, const auto& context) -> bool {
    return self.pending && self.ticket.queue != vir::QueueID::Invalid && context.is_retired(self.ticket);
  }
};
} // namespace ox
