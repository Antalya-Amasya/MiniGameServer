#include "minigame/Connection.hpp"

#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"
#include "minigame/Reactor.hpp"
#include "minigame/Session.hpp"

#include <chrono>
#include <string>

#include <sys/epoll.h>

namespace minigame {
namespace {

std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::atomic<std::uint64_t> Connection::nextId_{1};

Connection::Connection(TcpSocket socket, RoomManager& rooms, Reactor& reactor, ThreadPool& pool)
    : socket_(std::move(socket)),
      rooms_(rooms),
      reactor_(reactor),
      input_(proto::kMaxLineLength),
      session_(std::make_shared<Session>(rooms_)),
      lane_(SessionLane::create(pool, kMaxQueuedCommands)),
      id_(nextId_.fetch_add(1)),
      lastActivityMs_(nowMs()) {
  const std::uint64_t connectionId = id_;
  Reactor* reactorPointer = &reactor_;
  session_->setOutbound([reactorPointer, connectionId](std::string_view text) {
    reactorPointer->postResponse(connectionId, std::string(text), false);
  });
  log::info("accepted session id=" + std::to_string(session_->id()) +
            " connection=" + std::to_string(id_));
}

Connection::~Connection() {
  socket_.close();
}

std::uint64_t Connection::id() const {
  return id_;
}

int Connection::fd() const {
  return static_cast<int>(socket_.nativeHandle());
}

std::uint32_t Connection::interest() const {
  std::uint32_t events = EPOLLIN | EPOLLRDHUP;
  if (wantWrite_) {
    events |= EPOLLOUT;
  }
  return events;
}

bool Connection::isClosed() const {
  return closed_;
}

bool Connection::idleFor(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const {
  const std::int64_t seen = lastActivityMs_.load();
  const std::int64_t current = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  return current - seen >= timeout.count();
}

void Connection::onEvent(std::uint32_t events) {
  if (closed_) {
    return;
  }
  if ((events & (EPOLLIN | EPOLLRDHUP)) != 0) {
    onReadable();
  }
  if (closed_) {
    return;
  }
  if ((events & EPOLLOUT) != 0) {
    onWritable();
  }
  if (closed_) {
    return;
  }
  if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
    beginClose();
  }
}

void Connection::queueWrite(std::string_view bytes) {
  if (closed_ || bytes.empty()) {
    return;
  }
  if (!output_.tryAppend(bytes, kMaxOutputBytes)) {
    log::warn("output buffer limit connection=" + std::to_string(id_));
    beginClose();
    return;
  }
  flushOut();
}

void Connection::requestCloseAfterFlush() {
  if (closed_) {
    return;
  }
  closeAfterFlush_ = true;
  flushOut();
}

void Connection::onReadable() {
  char buffer[4096];
  while (!closed_) {
    const int received = socket_.recvSome(buffer, static_cast<int>(sizeof(buffer)));
    if (received == -2) {
      break;
    }
    if (received <= 0) {
      beginClose();
      return;
    }
    touch();
    if (input_.append(std::string_view(buffer, static_cast<std::size_t>(received))) == LineBuffer::Status::TooLong) {
      queueWrite(proto::error(proto::kInvalidCommand, "line too long"));
      closeAfterFlush_ = true;
      break;
    }
  }
  deliverLines();
  flushOut();
}

void Connection::onWritable() {
  flushOut();
}

void Connection::flushOut() {
  if (closed_) {
    return;
  }
  const OutputBuffer::Flush result = output_.flush([this](const char* data, int length) {
    return socket_.sendSome(data, length);
  });
  if (result == OutputBuffer::Flush::Error) {
    beginClose();
    return;
  }
  const bool needWrite = !output_.empty();
  if (needWrite != wantWrite_) {
    wantWrite_ = needWrite;
    reactor_.updateInterest(*this);
  }
  if (closeAfterFlush_ && output_.empty()) {
    beginClose();
  }
}

void Connection::beginClose() {
  if (closed_) {
    return;
  }
  closed_ = true;
  std::shared_ptr<Session> session = session_;
  if (!session) {
    return;
  }
  if (!lane_->post([session]() { session->finish(); })) {
    session->finish();
  }
}

void Connection::deliverLines() {
  std::string line;
  while (!closed_ && input_.next(line)) {
    if (line.find_first_not_of(" \t") == std::string::npos) {
      continue;
    }
    std::shared_ptr<Session> session = session_;
    const std::uint64_t connectionId = id_;
    Reactor* reactorPointer = &reactor_;
    const bool queued = lane_->post([session, line, connectionId, reactorPointer]() {
      if (session->handleLine(line)) {
        reactorPointer->postResponse(connectionId, "", true);
      }
    });
    if (!queued) {
      queueWrite(proto::error(proto::kServerBusy, "too many pending commands"));
      closeAfterFlush_ = true;
      break;
    }
  }
}

void Connection::touch() {
  lastActivityMs_.store(nowMs());
}

}  // namespace minigame
