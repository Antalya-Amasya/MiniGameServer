#pragma once

#include "minigame/LineBuffer.hpp"
#include "minigame/OutputBuffer.hpp"
#include "minigame/SessionLane.hpp"
#include "minigame/TcpSocket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace minigame {

class Reactor;
class RoomManager;
class Session;
class ThreadPool;

// 一条已接受的非阻塞连接。fd 只放在 socket_ 里。
// 业务任务进 SessionLane。回包不在 worker 里写 socket。
class Connection {
 public:
  static constexpr std::size_t kMaxOutputBytes = 256 * 1024;
  static constexpr std::size_t kMaxQueuedCommands = 32;

  Connection(TcpSocket socket, RoomManager& rooms, Reactor& reactor, ThreadPool& pool);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  std::uint64_t id() const;
  int fd() const;
  std::uint32_t interest() const;
  bool isClosed() const;
  bool idleFor(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const;

  void onEvent(std::uint32_t events);
  void queueWrite(std::string_view bytes);
  void requestCloseAfterFlush();

 private:
  void onReadable();
  void onWritable();
  void flushOut();
  void beginClose();
  void deliverLines();
  void touch();

  TcpSocket socket_;
  RoomManager& rooms_;
  Reactor& reactor_;
  LineBuffer input_;
  OutputBuffer output_;
  std::shared_ptr<Session> session_;
  std::shared_ptr<SessionLane> lane_;
  std::uint64_t id_ = 0;
  std::atomic<std::int64_t> lastActivityMs_{0};
  bool wantWrite_ = false;
  bool closeAfterFlush_ = false;
  bool closed_ = false;

  static std::atomic<std::uint64_t> nextId_;
};

}  // namespace minigame
