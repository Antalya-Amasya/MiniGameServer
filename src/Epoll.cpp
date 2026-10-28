#include "minigame/Epoll.hpp"

#include "minigame/Logger.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/epoll.h>
#include <unistd.h>

namespace minigame {

Epoll::Epoll() {
  fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd_ < 0) {
    log::error(std::string("epoll_create1 failed: ") + std::strerror(errno));
  }
}

Epoll::~Epoll() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool Epoll::valid() const {
  return fd_ >= 0;
}

bool Epoll::add(int fd, std::uint32_t events, void* token) {
  epoll_event event{};
  event.events = events;
  event.data.ptr = token;
  if (::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
    log::error(std::string("epoll_ctl ADD failed: ") + std::strerror(errno));
    return false;
  }
  return true;
}

bool Epoll::modify(int fd, std::uint32_t events, void* token) {
  epoll_event event{};
  event.events = events;
  event.data.ptr = token;
  if (::epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &event) != 0) {
    log::error(std::string("epoll_ctl MOD failed: ") + std::strerror(errno));
    return false;
  }
  return true;
}

bool Epoll::remove(int fd) {
  // 关闭顺序是先 DEL 再 close。ENOENT 说明已经不在集合里，不算失败。
  if (::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr) != 0 && errno != ENOENT) {
    log::warn(std::string("epoll_ctl DEL failed: ") + std::strerror(errno));
    return false;
  }
  return true;
}

int Epoll::wait(void* events, int capacity, int timeoutMs) {
  for (;;) {
    const int ready = ::epoll_wait(fd_, static_cast<epoll_event*>(events), capacity, timeoutMs);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    return ready;
  }
}

}  // namespace minigame
