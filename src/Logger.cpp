#include "minigame/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

namespace minigame {
namespace log {
namespace {

std::mutex g_logMutex;
bool g_enabled = true;

std::string timestamp() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm localTime{};
#ifdef _WIN32
  localtime_s(&localTime, &now);
#else
  localtime_r(&now, &localTime);
#endif
  char buffer[32] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime) == 0) {
    return "0000-00-00 00:00:00";
  }
  return buffer;
}

void write(const char* level, std::string_view message) {
  // 开关和时间格式化都放在同一把锁里，避免和 setEnabled 产生数据竞争。
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (!g_enabled) {
    return;
  }
  std::cerr << '[' << timestamp() << "] [" << level << "] " << message << '\n' << std::flush;
}

}  // namespace

void setEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_enabled = enabled;
}

void info(std::string_view message) {
  write("INFO", message);
}

void warn(std::string_view message) {
  write("WARN", message);
}

void error(std::string_view message) {
  write("ERROR", message);
}

}  // namespace log
}  // namespace minigame
