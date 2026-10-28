#pragma once

#include "minigame/PlayerLink.hpp"
#include "minigame/TcpSocket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace minigame {

class RoomManager;

// 一个 TCP 连接对应一个 Session。
// Windows 上仍由 Session::run() 在自己的线程里阻塞读。
// Linux Reactor 上不再启动读线程：Connection 切出一行后调用 handleLine()。
//
// 所有权：
// - Session 由 shared_ptr 管理。
// - 房间只保存 weak_ptr。
// - Windows 上 socket_ 属于 Session。Linux Reactor 上 socket 属于 Connection，Session 只持有发送回调。
class Session : public PlayerLink, public std::enable_shared_from_this<Session> {
 public:
  Session(TcpSocket socket, RoomManager& rooms);
  explicit Session(RoomManager& rooms);
  ~Session() override;

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void run();
  void requestStop();
  void setOutbound(std::function<void(std::string_view)> outbound);
  bool handleLine(const std::string& line);
  void finish();

  std::uint64_t id() const override;
  const std::string& name() const override;
  std::uint64_t accountId() const override;
  void send(std::string_view text) override;
  void onRoomChanged(int roomId) override;

  int roomId() const;
  bool online() const;

 private:
  void readLoop();
  // 返回 true 表示读循环应该结束（QUIT，或必须断开的坏输入已在 readLoop 里处理）。
  bool dispatch(const std::string& line);

  std::mutex sendMu_;
  TcpSocket socket_;
  RoomManager& rooms_;
  std::uint64_t id_ = 0;
  std::uint64_t accountId_ = 0;
  std::string name_;
  std::atomic<int> roomId_{0};
  std::atomic<bool> online_{false};
  std::atomic<bool> closing_{false};
  std::mutex commandMu_;
  bool finished_ = false;
  std::function<void(std::string_view)> outbound_;

  static std::atomic<std::uint64_t> nextId_;
};

}  // namespace minigame
