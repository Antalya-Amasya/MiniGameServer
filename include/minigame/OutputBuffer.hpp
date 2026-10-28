#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace minigame {

// 尚未写完的输出。flush 的写入函数一次可以只消化一部分。
// 返回值：>0 写了多少字节，-2 暂时写不了，<=0 连接失败。
class OutputBuffer {
 public:
  enum class Flush { Done, Again, Error };

  void append(std::string_view bytes);
  bool tryAppend(std::string_view bytes, std::size_t maxBytes);
  bool empty() const;
  std::size_t size() const;

  Flush flush(const std::function<int(const char*, int)>& writeSome);

 private:
  std::string data_;
};

}  // namespace minigame
