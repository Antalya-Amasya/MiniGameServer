#pragma once

#include <cstdint>

namespace minigame {

// epoll fd 的 RAII 包装。只在 Linux 上编译。
class Epoll {
 public:
  Epoll();
  ~Epoll();

  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  bool valid() const;
  bool add(int fd, std::uint32_t events, void* token);
  bool modify(int fd, std::uint32_t events, void* token);
  bool remove(int fd);

  // 一直等到有事件、被信号打断，或出错。timeoutMs < 0 表示无限等待。
  int wait(void* events, int capacity, int timeoutMs);

 private:
  int fd_ = -1;
};

}  // namespace minigame
