#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace minigame {

struct PendingResponse {
  std::uint64_t connectionId = 0;
  std::string bytes;
  bool closeAfterWrite = false;
};

// Worker 只往这里放结果。Reactor 线程再 drain。
class ResponseQueue {
 public:
  explicit ResponseQueue(std::size_t maxQueued);

  bool push(PendingResponse response);
  std::vector<PendingResponse> drain();

 private:
  std::mutex mutex_;
  std::vector<PendingResponse> items_;
  std::size_t maxQueued_;
};

}  // namespace minigame
