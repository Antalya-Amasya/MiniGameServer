#include "minigame/SessionLane.hpp"

#include "minigame/Logger.hpp"

#include <exception>
#include <string>
#include <utility>

namespace minigame {

std::shared_ptr<SessionLane> SessionLane::create(ThreadPool& pool, std::size_t maxQueued) {
  return std::make_shared<SessionLane>(pool, maxQueued);
}

SessionLane::SessionLane(ThreadPool& pool, std::size_t maxQueued) : pool_(pool), maxQueued_(maxQueued) {}

bool SessionLane::post(std::function<void()> job) {
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= maxQueued_) {
      return false;
    }
    queue_.push_back(std::move(job));
    if (!running_) {
      running_ = true;
      start = true;
    }
  }
  if (!start) {
    return true;
  }
  std::shared_ptr<SessionLane> self = shared_from_this();
  if (!pool_.submit([self]() { self->drain(); })) {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    queue_.clear();
    return false;
  }
  return true;
}

void SessionLane::drain() {
  while (true) {
    std::function<void()> job;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty()) {
        running_ = false;
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      job();
    } catch (const std::exception& ex) {
      log::error(std::string("session task exception: ") + ex.what());
    } catch (...) {
      log::error("session task unknown exception");
    }
  }
}

}  // namespace minigame
