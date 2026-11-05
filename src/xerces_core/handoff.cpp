#include "handoff.hpp"

#include <exception>
#include <utility>

namespace xerces_core {
handoff::handoff(std::function<std::string()> fn)
    : fn_(std::move(fn)), result_(promise_.get_future()) {}

bool handoff::run() {
  state expected = state::pending;
  if (!state_.compare_exchange_strong(expected, state::running)) {
    return false; // the waiter was told "busy"; do not act late
  }
  try {
    promise_.set_value(fn_());
  } catch (...) {
    promise_.set_exception(std::current_exception());
  }
  return true;
}

std::optional<std::string> handoff::wait(std::chrono::milliseconds timeout) {
  if (result_.wait_for(timeout) != std::future_status::ready) {
    state expected = state::pending;
    if (state_.compare_exchange_strong(expected, state::cancelled)) {
      return std::nullopt; // it never started, and now it never will
    }
    // The worker took the task before we gave up, so it is running (or just
    // finished) and what it does will happen: wait for it rather than answer
    // "busy" for a change that is being made.
  }
  return result_.get();
}
} // namespace xerces_core
