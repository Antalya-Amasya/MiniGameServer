#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace minigame {

// 固定数量的工作线程。stop() 会把已经入队的任务做完，然后 join。
// 新任务在 stop 之后会被拒绝。
class ThreadPool {
 public:
  ThreadPool(std::size_t workerCount, std::size_t maxQueued);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  bool submit(std::function<void()> task);
  void stop();

  std::size_t workerCount() const;
  std::size_t queuedForTest() const;

 private:
  void workerLoop();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  std::size_t maxQueued_;
  bool stopping_ = false;
  bool joined_ = false;
};

std::size_t defaultWorkerCount();

}  // namespace minigame
