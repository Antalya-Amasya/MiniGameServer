#include "minigame/Reactor.hpp"

#include "minigame/Connection.hpp"
#include "minigame/Logger.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace minigame {

char Reactor::listenToken_ = 0;
char Reactor::wakeToken_ = 0;
char Reactor::timerToken_ = 0;

namespace {

int timerIntervalMs(std::chrono::milliseconds idleTimeout) {
  std::int64_t interval = idleTimeout.count() / 2;
  if (interval < 100) {
    interval = 100;
  }
  if (interval > 1000) {
    interval = 1000;
  }
  return static_cast<int>(interval);
}

}  // namespace

Reactor::Reactor(TcpSocket listenSocket, RoomManager& rooms, std::chrono::milliseconds idleTimeout)
    : listenSocket_(std::move(listenSocket)),
      rooms_(rooms),
      pool_(defaultWorkerCount(), 1024),
      responses_(1024),
      idleTimeout_(idleTimeout) {
  log::info("thread pool workers=" + std::to_string(pool_.workerCount()));
}

Reactor::~Reactor() {
  shutdownPool();
  destroyAll();
  if (listenSocket_.valid()) {
    epoll_.remove(static_cast<int>(listenSocket_.nativeHandle()));
    listenSocket_.close();
  }
  if (timerFd_ >= 0) {
    epoll_.remove(timerFd_);
    ::close(timerFd_);
    timerFd_ = -1;
  }
  if (eventFd_ >= 0) {
    epoll_.remove(eventFd_);
    ::close(eventFd_);
    eventFd_ = -1;
  }
}

bool Reactor::arm() {
  if (!epoll_.valid() || !listenSocket_.valid()) {
    return false;
  }
  eventFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (eventFd_ < 0) {
    log::error(std::string("eventfd failed: ") + std::strerror(errno));
    return false;
  }
  if (!armTimer()) {
    return false;
  }
  const int listenFd = static_cast<int>(listenSocket_.nativeHandle());
  if (!epoll_.add(listenFd, EPOLLIN, &listenToken_)) {
    return false;
  }
  if (!epoll_.add(eventFd_, EPOLLIN, &wakeToken_)) {
    return false;
  }
  if (!epoll_.add(timerFd_, EPOLLIN, &timerToken_)) {
    return false;
  }
  return true;
}

bool Reactor::armTimer() {
  timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerFd_ < 0) {
    log::error(std::string("timerfd_create failed: ") + std::strerror(errno));
    return false;
  }
  const int interval = timerIntervalMs(idleTimeout_);
  itimerspec spec{};
  spec.it_interval.tv_sec = interval / 1000;
  spec.it_interval.tv_nsec = static_cast<long>((interval % 1000) * 1000000L);
  spec.it_value = spec.it_interval;
  if (::timerfd_settime(timerFd_, 0, &spec, nullptr) != 0) {
    log::error(std::string("timerfd_settime failed: ") + std::strerror(errno));
    return false;
  }
  return true;
}

void Reactor::loop() {
  log::info("reactor loop started");
  while (!stopping_) {
    epoll_event events[64];
    const int ready = epoll_.wait(events, 64, -1);
    if (ready < 0) {
      log::error(std::string("epoll_wait failed: ") + std::strerror(errno));
      break;
    }

    bool acceptNow = false;
    for (int index = 0; index < ready; ++index) {
      void* token = events[index].data.ptr;
      if (token == &wakeToken_) {
        std::uint64_t value = 0;
        while (::read(eventFd_, &value, sizeof(value)) > 0) {
        }
        continue;
      }
      if (token == &timerToken_) {
        std::uint64_t expirations = 0;
        while (::read(timerFd_, &expirations, sizeof(expirations)) > 0) {
        }
        checkIdle();
        continue;
      }
      if (token == &listenToken_) {
        acceptNow = true;
        continue;
      }
      auto* connection = static_cast<Connection*>(token);
      connection->onEvent(events[index].events);
    }

    applyResponses();
    collectClosed();
    if (stopping_) {
      break;
    }
    if (acceptNow) {
      acceptReady();
    }
  }

  shutdownPool();
  applyResponses();
  collectClosed();
  destroyAll();
  if (listenSocket_.valid()) {
    epoll_.remove(static_cast<int>(listenSocket_.nativeHandle()));
    listenSocket_.close();
  }
  if (timerFd_ >= 0) {
    epoll_.remove(timerFd_);
    ::close(timerFd_);
    timerFd_ = -1;
  }
  if (eventFd_ >= 0) {
    epoll_.remove(eventFd_);
    ::close(eventFd_);
    eventFd_ = -1;
  }
  log::info("reactor loop stopped");
}

void Reactor::wake() {
  if (eventFd_ < 0) {
    return;
  }
  const std::uint64_t value = 1;
  const ssize_t written = ::write(eventFd_, &value, sizeof(value));
  if (written < 0 && errno != EAGAIN) {
    log::warn(std::string("eventfd write failed: ") + std::strerror(errno));
  }
}

void Reactor::requestStop() {
  stopping_ = true;
  wake();
}

void Reactor::updateInterest(Connection& connection) {
  if (connection.isClosed() || connection.fd() < 0) {
    return;
  }
  epoll_.modify(connection.fd(), connection.interest(), &connection);
}

void Reactor::postResponse(std::uint64_t connectionId, std::string bytes, bool closeAfterWrite) {
  PendingResponse response;
  response.connectionId = connectionId;
  response.bytes = std::move(bytes);
  response.closeAfterWrite = closeAfterWrite;
  if (!responses_.push(std::move(response))) {
    log::warn("response queue full");
    return;
  }
  wake();
}

std::size_t Reactor::workerCount() const {
  return pool_.workerCount();
}

void Reactor::acceptReady() {
  if (stopping_) {
    return;
  }
  for (;;) {
    TcpSocket client = listenSocket_.acceptClient();
    if (!client.valid()) {
      return;
    }
    if (!client.setNonBlocking()) {
      log::warn("failed to set accepted socket non-blocking");
      continue;
    }
    const int fd = static_cast<int>(client.nativeHandle());
    std::shared_ptr<Connection> connection =
        std::make_shared<Connection>(std::move(client), rooms_, *this, pool_);
    if (!epoll_.add(fd, connection->interest(), connection.get())) {
      connection->onEvent(EPOLLERR);
      continue;
    }
    connections_.emplace(connection->id(), std::move(connection));
  }
}

void Reactor::applyResponses() {
  const std::vector<PendingResponse> batch = responses_.drain();
  for (const PendingResponse& response : batch) {
    const auto found = connections_.find(response.connectionId);
    if (found == connections_.end()) {
      continue;
    }
    Connection& connection = *found->second;
    if (connection.id() != response.connectionId || connection.isClosed()) {
      continue;
    }
    if (!response.bytes.empty()) {
      connection.queueWrite(response.bytes);
    }
    if (response.closeAfterWrite) {
      connection.requestCloseAfterFlush();
    }
  }
}

void Reactor::checkIdle() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::uint64_t> expired;
  for (const auto& entry : connections_) {
    if (entry.second->idleFor(now, idleTimeout_)) {
      expired.push_back(entry.first);
    }
  }
  for (std::uint64_t id : expired) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
      continue;
    }
    log::info("idle timeout connection=" + std::to_string(id));
    found->second->onEvent(EPOLLERR);
  }
}

void Reactor::destroyConnection(std::uint64_t connectionId) {
  const auto found = connections_.find(connectionId);
  if (found == connections_.end()) {
    return;
  }
  const int fd = found->second->fd();
  if (fd >= 0) {
    epoll_.remove(fd);
  }
  connections_.erase(found);
}

void Reactor::collectClosed() {
  std::vector<std::uint64_t> ids;
  for (const auto& entry : connections_) {
    if (entry.second->isClosed()) {
      ids.push_back(entry.first);
    }
  }
  for (std::uint64_t id : ids) {
    destroyConnection(id);
  }
}

void Reactor::destroyAll() {
  std::vector<std::uint64_t> ids;
  ids.reserve(connections_.size());
  for (const auto& entry : connections_) {
    ids.push_back(entry.first);
  }
  for (std::uint64_t id : ids) {
    destroyConnection(id);
  }
}

void Reactor::shutdownPool() {
  if (poolStopped_) {
    return;
  }
  poolStopped_ = true;
  pool_.stop();
}

}  // namespace minigame
