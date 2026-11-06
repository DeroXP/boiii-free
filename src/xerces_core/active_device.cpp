#include "active_device.hpp"

namespace xerces_core {
active_device_filter::verdict
active_device_filter::check(const std::int32_t local_client,
                            const std::int32_t count) {
  if (local_client_in_range(local_client, count)) {
    return verdict::call;
  }
  return dropped_.fetch_add(1, std::memory_order_relaxed) == 0
             ? verdict::drop_first
             : verdict::drop;
}
} // namespace xerces_core
