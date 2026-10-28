#include "minigame/OutputBuffer.hpp"
#include "minigame/ThreadPool.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

int g_failed = 0;

void require(bool condition, const std::string& detail) {
  if (!condition) {
    ++g_failed;
    std::cout << "FAIL " << detail << "\n";
    return;
  }
  std::cout << "PASS " << detail << "\n";
}

void testTaskQueueLimit() {
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  minigame::ThreadPool pool(1, 2);
  require(pool.submit([&started, &release]() {
            started.store(true);
            while (!release.load()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          }),
          "first task accepted");
  while (!started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(pool.submit([]() {}), "queue slot 1");
  require(pool.submit([]() {}), "queue slot 2");
  require(!pool.submit([]() {}), "queue rejects past the cap");
  release.store(true);
}

void testOutputCap() {
  minigame::OutputBuffer buffer;
  const std::string chunk(1000, 'x');
  int accepted = 0;
  while (buffer.tryAppend(chunk, 4096)) {
    ++accepted;
  }
  require(accepted > 0, "output buffer accepts data");
  require(buffer.size() <= 4096, "output buffer stays within the cap");
  require(!buffer.tryAppend(chunk, 4096), "output buffer rejects further growth");
}

}  // namespace

int main() {
  testTaskQueueLimit();
  testOutputCap();
  if (g_failed != 0) {
    std::cout << "phase5 limits failed=" << g_failed << "\n";
    return 1;
  }
  std::cout << "phase5 limits passed\n";
  return 0;
}
