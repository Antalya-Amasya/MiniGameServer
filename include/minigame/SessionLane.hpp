#pragma once

#include "minigame/ThreadPool.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace minigame {

// 同一个 Session 的任务按提交顺序执行，不会同时进两个 worker。
class SessionLane : public std::enable_shared_from_this<SessionLane> {
 public:
  static std::shared_ptr<SessionLane> create(ThreadPool& pool, std::size_t maxQueued);

  bool post(std::function<void()> job);

  SessionLane(ThreadPool& pool, std::size_t maxQueued);

 private:
  void drain();

  ThreadPool& pool_;
  std::size_t maxQueued_;
  std::mutex mutex_;
  std::deque<std::function<void()>> queue_;
  bool running_ = false;
};

}  // namespace minigame
