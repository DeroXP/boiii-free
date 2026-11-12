#include "handoff.hpp"

#include <algorithm>
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

std::optional<std::string> handoff::give_up_or_finish() {
  state expected = state::pending;
  if (state_.compare_exchange_strong(expected, state::cancelled)) {
    return std::nullopt; // it never started, and now it never will
  }
  // The worker took the task before we gave up, so it is running (or just
  // finished) and what it does will happen: wait for it rather than answer
  // "busy" for a change that is being made.
  return result_.get();
}

std::optional<std::string> handoff::wait(std::chrono::milliseconds timeout) {
  if (result_.wait_for(timeout) != std::future_status::ready) {
    return give_up_or_finish();
  }
  return result_.get();
}

std::optional<std::string> handoff::wait(std::chrono::milliseconds timeout,
                                         const std::function<bool()> &give_up,
                                         std::chrono::milliseconds step) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (step <= std::chrono::milliseconds::zero()) {
    step = std::chrono::milliseconds(1);
  }
  for (;;) {
    if (give_up && give_up()) {
      break;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= std::chrono::milliseconds::zero()) {
      // One last look, as wait(timeout) has with a timeout of 0
      if (result_.wait_for(std::chrono::milliseconds::zero()) ==
          std::future_status::ready) {
        return result_.get();
      }
      break;
    }
    if (result_.wait_for(std::min(step, left)) == std::future_status::ready) {
      return result_.get();
    }
  }
  return give_up_or_finish();
}
} // namespace xerces_core
