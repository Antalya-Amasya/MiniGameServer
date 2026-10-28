#pragma once

#include <string_view>

namespace minigame {
namespace log {

// 轻量日志：写到 stderr，用一把互斥锁串行化。
// Phase 1 不写文件，避免再引入一套配置。
void setEnabled(bool enabled);
void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);

}  // namespace log
}  // namespace minigame
