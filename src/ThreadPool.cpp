#include "minigame/ThreadPool.hpp"

#include "minigame/Logger.hpp"

#include <exception>
#include <string>

namespace minigame {

std::size_t defaultWorkerCount() {
  unsigned int hardware = std::thread::hardware_concurrency();
  if (hardware < 2) {
    hardware = 2;
  }
  // 井字棋的业务很短。线程太多只会让关闭和数据竞争更难讲清楚。
  if (hardware > 4) {
    hardware = 4;
  }
  return hardware;
}

ThreadPool::ThreadPool(std::size_t workerCount, std::size_t maxQueued) : maxQueued_(maxQueued) {
  if (workerCount == 0) {
    workerCount = 1;
  }
  workers_.reserve(workerCount);
  for (std::size_t index = 0; index < workerCount; ++index) {
    workers_.emplace_back([this]() { workerLoop(); });
  }
}

ThreadPool::~ThreadPool() {
  stop();
}

bool ThreadPool::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || tasks_.size() >= maxQueued_) {
      return false;
    }
    tasks_.push(std::move(task));
  }
  condition_.notify_one();
  return true;
}

void ThreadPool::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (joined_) {
      return;
    }
    stopping_ = true;
  }
  condition_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  joined_ = true;
}

std::size_t ThreadPool::workerCount() const {
  return workers_.size();
}

std::size_t ThreadPool::queuedForTest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

void ThreadPool::workerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    try {
      task();
    } catch (const std::exception& ex) {
      log::error(std::string("worker task exception: ") + ex.what());
    } catch (...) {
      log::error("worker task unknown exception");
    }
  }
}

}  // namespace minigame
