#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace minigame {

// 无效句柄在 Windows 和 Linux 上都用全 1 位表示（INVALID_SOCKET / -1）。
inline constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(-1);

// RAII socket。析构、移动和 close() 都会归还句柄，业务代码不直接 close。
// 句柄用 atomic 保存，使 close() 可以重复调用。
// 监听套接字只由 accept 线程自己 close；别的线程只用 shutdownBoth() 唤醒阻塞读。
class TcpSocket {
 public:
  TcpSocket();
  ~TcpSocket();

  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;

  bool valid() const;
  bool listenOn(std::uint16_t port, int backlog = 16);
  TcpSocket acceptClient();
  bool connectTo(const std::string& host, std::uint16_t port);
  bool setRecvTimeoutMs(int timeoutMs);
  bool setNonBlocking();
  // 只打断阻塞中的 recv/accept 相关等待，不释放句柄，避免 fd 被立刻复用。
  void shutdownBoth();
  std::uintptr_t nativeHandle() const;
  std::string lastError() const;

  // >0 读到的字节数，0 对端正常关闭，-1 出错，-2 超时。
  int recvSome(char* buffer, int capacity);
  // >0 本次写出的字节，-2 暂时写不了，-1 失败。
  int sendSome(const char* data, int length);
  bool sendAll(std::string_view data);
  void close();

  std::uint16_t boundPort() const;

 private:
  struct AdoptTag {};
  TcpSocket(std::uintptr_t native, AdoptTag);

  std::atomic<std::uintptr_t> handle_;
};

// 进程级 socket 运行时。Windows 上负责 WSAStartup / WSACleanup。
// 必须在创建任何 TcpSocket 之前构造，并活过最后一个套接字。
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();

  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;

  bool ok() const;

 private:
  bool ok_ = false;
};

// 本机互连的一对已连接套接字，用来唤醒 select。
// first 是 accept 得到的一端，second 是 connect 的一端。
bool createConnectedPair(TcpSocket& first, TcpSocket& second);

enum class IoWait { Ready, Woken, Error };

// 阻塞到 primary 或 wakeup 可读。只给 Server 的监听循环使用。
IoWait waitForReadable(const TcpSocket& primary, const TcpSocket& wakeup);

}  // namespace minigame
