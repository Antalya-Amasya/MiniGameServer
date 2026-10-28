#include "minigame/TcpSocket.hpp"

#include "minigame/Logger.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace minigame {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
#else
using NativeSocket = int;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

NativeSocket toNative(std::uintptr_t handle) {
  return static_cast<NativeSocket>(handle);
}

bool isInvalid(NativeSocket fd) {
#ifdef _WIN32
  return fd == INVALID_SOCKET;
#else
  return fd < 0;
#endif
}

std::string socketErrorText() {
#ifdef _WIN32
  return "winsock error " + std::to_string(WSAGetLastError());
#else
  return std::string("errno ") + std::to_string(errno) + " " + std::strerror(errno);
#endif
}

void closeNative(NativeSocket fd) {
  if (isInvalid(fd)) {
    return;
  }
#ifdef _WIN32
  ::shutdown(fd, SD_BOTH);
  ::closesocket(fd);
#else
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
#endif
}

void configureConnected(NativeSocket fd) {
  const int enabled = 1;
  const char* option = reinterpret_cast<const char*>(&enabled);
  // 协议消息很短。关掉 Nagle，避免落子结果在缓冲区里多等一个往返。
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, option, static_cast<int>(sizeof(enabled)));
}

bool setReuse(NativeSocket fd) {
  const char* option = nullptr;
  int enabled = 1;
#ifdef _WIN32
  // Windows 上 SO_REUSEADDR 会允许第二个进程绑到同一个端口。
  // 这里用独占模式，bind 失败才能被测试观察到。
  option = reinterpret_cast<const char*>(&enabled);
  return ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, option, static_cast<int>(sizeof(enabled))) == 0;
#else
  option = reinterpret_cast<const char*>(&enabled);
  return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, option, static_cast<int>(sizeof(enabled))) == 0;
#endif
}

}  // namespace

TcpSocket::TcpSocket() : handle_(kInvalidSocket) {}

TcpSocket::TcpSocket(std::uintptr_t native, AdoptTag) : handle_(native) {
  if (native != kInvalidSocket) {
    configureConnected(toNative(native));
  }
}

TcpSocket::~TcpSocket() {
  close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_.exchange(kInvalidSocket)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    handle_.store(other.handle_.exchange(kInvalidSocket));
  }
  return *this;
}

bool TcpSocket::valid() const {
  return handle_.load() != kInvalidSocket;
}

bool TcpSocket::listenOn(std::uint16_t port, int backlog) {
  close();
  const NativeSocket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (isInvalid(fd)) {
    log::error("socket failed: " + socketErrorText());
    return false;
  }
  if (!setReuse(fd)) {
    log::warn("address reuse option failed: " + socketErrorText());
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    log::error("bind failed on port " + std::to_string(port) + ": " + socketErrorText());
    closeNative(fd);
    return false;
  }
  if (::listen(fd, backlog) != 0) {
    log::error("listen failed: " + socketErrorText());
    closeNative(fd);
    return false;
  }

  handle_.store(static_cast<std::uintptr_t>(fd));
  return true;
}

TcpSocket TcpSocket::acceptClient() {
  const std::uintptr_t listenFd = handle_.load();
  if (listenFd == kInvalidSocket) {
    return TcpSocket();
  }

  for (;;) {
    const NativeSocket accepted = ::accept(toNative(listenFd), nullptr, nullptr);
    if (!isInvalid(accepted)) {
      return TcpSocket(static_cast<std::uintptr_t>(accepted), AdoptTag{});
    }
#ifndef _WIN32
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return TcpSocket();
    }
#endif
    // stop() 关闭监听套接字时，accept 失败是正常路径，不记成错误。
    if (handle_.load() != kInvalidSocket) {
      log::warn("accept failed: " + socketErrorText());
    }
    return TcpSocket();
  }
}

bool TcpSocket::connectTo(const std::string& host, std::uint16_t port) {
  close();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  const std::string portText = std::to_string(port);
  if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
    log::error("getaddrinfo failed for " + host);
    return false;
  }

  bool connected = false;
  for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
    const NativeSocket fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (isInvalid(fd)) {
      continue;
    }
    if (::connect(fd, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0) {
      handle_.store(static_cast<std::uintptr_t>(fd));
      configureConnected(fd);
      connected = true;
      break;
    }
    closeNative(fd);
  }
  ::freeaddrinfo(result);
  if (!connected) {
    log::error("connect failed " + host + ":" + portText + ": " + socketErrorText());
  }
  return connected;
}

bool TcpSocket::setRecvTimeoutMs(int timeoutMs) {
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return false;
  }
  if (timeoutMs < 0) {
    timeoutMs = 0;
  }
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(timeoutMs);
  const char* option = reinterpret_cast<const char*>(&timeout);
  return ::setsockopt(toNative(current), SOL_SOCKET, SO_RCVTIMEO, option, static_cast<int>(sizeof(timeout))) == 0;
#else
  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  return ::setsockopt(toNative(current), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

bool TcpSocket::setNonBlocking() {
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return false;
  }
#ifdef _WIN32
  u_long mode = 1;
  return ::ioctlsocket(toNative(current), FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(toNative(current), F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(toNative(current), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int TcpSocket::sendSome(const char* data, int length) {
  if (data == nullptr || length <= 0) {
    return -1;
  }
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return -1;
  }
  for (;;) {
    const int sent = ::send(toNative(current), data, length, MSG_NOSIGNAL);
    if (sent > 0) {
      return sent;
    }
#ifdef _WIN32
    const int code = WSAGetLastError();
    if (code == WSAEINTR) {
      continue;
    }
    if (code == WSAEWOULDBLOCK) {
      return -2;
    }
    return -1;
#else
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -2;
    }
    return -1;
#endif
  }
}

int TcpSocket::recvSome(char* buffer, int capacity) {
  if (buffer == nullptr || capacity <= 0) {
    return -1;
  }
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return -1;
  }

  for (;;) {
    const int received = ::recv(toNative(current), buffer, capacity, 0);
    if (received > 0) {
      return received;
    }
    if (received == 0) {
      return 0;
    }
#ifdef _WIN32
    const int code = WSAGetLastError();
    if (code == WSAEINTR) {
      continue;
    }
    if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) {
      return -2;
    }
#else
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -2;
    }
#endif
    return -1;
  }
}

bool TcpSocket::sendAll(std::string_view data) {
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return false;
  }

  std::size_t sentTotal = 0;
  while (sentTotal < data.size()) {
    const int sent = ::send(
        toNative(current),
        data.data() + sentTotal,
        static_cast<int>(data.size() - sentTotal),
        MSG_NOSIGNAL);
    if (sent > 0) {
      sentTotal += static_cast<std::size_t>(sent);
      continue;
    }
#ifndef _WIN32
    if (sent < 0 && errno == EINTR) {
      continue;
    }
#endif
    return false;
  }
  return true;
}

void TcpSocket::close() {
  const std::uintptr_t previous = handle_.exchange(kInvalidSocket);
  if (previous == kInvalidSocket) {
    return;
  }
  closeNative(toNative(previous));
}

void TcpSocket::shutdownBoth() {
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::shutdown(toNative(current), SD_BOTH);
#else
  ::shutdown(toNative(current), SHUT_RDWR);
#endif
}

std::uintptr_t TcpSocket::nativeHandle() const {
  return handle_.load();
}

std::string TcpSocket::lastError() const {
  return socketErrorText();
}

std::uint16_t TcpSocket::boundPort() const {
  const std::uintptr_t current = handle_.load();
  if (current == kInvalidSocket) {
    return 0;
  }
  sockaddr_in address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(toNative(current), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

SocketRuntime::SocketRuntime() {
#ifdef _WIN32
  WSADATA data{};
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  if (!ok_) {
    log::error("WSAStartup failed: " + socketErrorText());
  }
#else
  ok_ = true;
#endif
}

SocketRuntime::~SocketRuntime() {
#ifdef _WIN32
  if (ok_) {
    ::WSACleanup();
  }
#endif
}

bool SocketRuntime::ok() const {
  return ok_;
}

bool createConnectedPair(TcpSocket& first, TcpSocket& second) {
  TcpSocket listener;
  if (!listener.listenOn(0)) {
    return false;
  }
  const std::uint16_t port = listener.boundPort();
  if (port == 0) {
    return false;
  }

  TcpSocket connecting;
  std::atomic<bool> connected{false};
  std::thread connector([&connecting, &connected, port]() {
    connected.store(connecting.connectTo("127.0.0.1", port));
  });

  bool readable = false;
  const std::uintptr_t listenFd = listener.nativeHandle();
  if (listenFd != kInvalidSocket) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(toNative(listenFd), &readSet);
    timeval timeout{};
    timeout.tv_sec = 2;
#ifdef _WIN32
    readable = ::select(0, &readSet, nullptr, nullptr, &timeout) > 0;
#else
    readable = ::select(static_cast<int>(listenFd) + 1, &readSet, nullptr, nullptr, &timeout) > 0;
#endif
  }

  TcpSocket accepted;
  if (readable) {
    accepted = listener.acceptClient();
  }
  connector.join();
  if (!connected.load() || !accepted.valid()) {
    log::error("wakeup pair setup failed");
    return false;
  }
  char probeByte = 0;

  first = std::move(accepted);
  second = std::move(connecting);
  if (!second.sendAll("x")) {
    log::error("wakeup pair send failed: " + socketErrorText());
    return false;
  }
  const int received = first.recvSome(&probeByte, 1);
  if (received != 1 || probeByte != 'x') {
    log::error("wakeup pair recv failed: " + socketErrorText());
    return false;
  }
  return true;
}

IoWait waitForReadable(const TcpSocket& primary, const TcpSocket& wakeup) {
  const std::uintptr_t primaryFd = primary.nativeHandle();
  const std::uintptr_t wakeupFd = wakeup.nativeHandle();
  if (primaryFd == kInvalidSocket || wakeupFd == kInvalidSocket) {
    return IoWait::Error;
  }

  for (;;) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(toNative(primaryFd), &readSet);
    FD_SET(toNative(wakeupFd), &readSet);
#ifdef _WIN32
    const int ready = ::select(0, &readSet, nullptr, nullptr, nullptr);
#else
    const int primaryInt = static_cast<int>(primaryFd);
    const int wakeupInt = static_cast<int>(wakeupFd);
    const int maxFd = primaryInt > wakeupInt ? primaryInt : wakeupInt;
    const int ready = ::select(maxFd + 1, &readSet, nullptr, nullptr, nullptr);
#endif
    if (ready > 0) {
      if (FD_ISSET(toNative(primaryFd), &readSet)) {
        return IoWait::Ready;
      }
      return IoWait::Woken;
    }
#ifdef _WIN32
    if (ready < 0 && WSAGetLastError() == WSAEINTR) {
      continue;
    }
#else
    if (ready < 0 && errno == EINTR) {
      continue;
    }
#endif
    return IoWait::Error;
  }
}

}  // namespace minigame
