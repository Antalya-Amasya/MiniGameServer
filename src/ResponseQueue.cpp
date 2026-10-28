#include "minigame/ResponseQueue.hpp"

namespace minigame {

ResponseQueue::ResponseQueue(std::size_t maxQueued) : maxQueued_(maxQueued) {}

bool ResponseQueue::push(PendingResponse response) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (items_.size() >= maxQueued_) {
    return false;
  }
  items_.push_back(std::move(response));
  return true;
}

std::vector<PendingResponse> ResponseQueue::drain() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PendingResponse> batch;
  batch.swap(items_);
  return batch;
}

}  // namespace minigame
