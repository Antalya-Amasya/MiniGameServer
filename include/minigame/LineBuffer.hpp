#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace minigame {

// 把 TCP 字节流切成以 '\n' 结尾的文本帧。
// 一次 append 可以包含多条命令，也可以只包含半条命令。
class LineBuffer {
 public:
  explicit LineBuffer(std::size_t maxLineLength);

  enum class Status { Ok, TooLong };

  Status append(std::string_view bytes);
  bool next(std::string& line);
  std::size_t pending() const;

 private:
  std::size_t maxLineLength_;
  std::string data_;
};

}  // namespace minigame
