#pragma once

// A call one thread hands to another and may stop waiting for. The game's
// command pipe (src/client/xerces/ipc.cpp, docs/DESIGN.md 15.2) hands each
// request to the main thread and answers "busy" when the main thread cannot
// take it within two seconds. That answer must be true: a request answered
// "busy" never runs, and a request the main thread has already started is
// waited for and answered with its real result. The task's state is pending,
// running or cancelled, and only one thread wins the move out of pending.

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <string>

namespace xerces_core {
class handoff {
public:
  explicit handoff(std::function<std::string()> fn);
  handoff(const handoff &) = delete;
  handoff &operator=(const handoff &) = delete;

  // On the thread that does the work: runs the task unless the waiter has
  // already given up. False if it did not run.
  bool run();

  // On the waiting thread, once: the task's result, or nothing if it had not
  // started within `timeout` (it then never will). A task that started in
  // time is waited for to the end, however close to the deadline it began;
  // callers only hand over short tasks. An exception the task threw is thrown
  // here.
  std::optional<std::string> wait(std::chrono::milliseconds timeout);

private:
  enum class state { pending, running, cancelled };

  std::function<std::string()> fn_;
  std::atomic<state> state_ = state::pending;
  std::promise<std::string> promise_;
  std::future<std::string> result_;
};
} // namespace xerces_core
