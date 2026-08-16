#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace lwweb {

enum class ProxyStreamReadResult { Data, Finished, Failed };

// 在后台 HTTP 客户端和本地响应之间提供有界、线程安全的字节队列。
// 写端在队列满时等待，从而把 WebView/磁盘的背压传递到真实后台连接。
class ProxyStream {
 public:
  explicit ProxyStream(std::size_t capacity = 1024 * 1024);

  bool Push(const char* data, std::size_t length);
  ProxyStreamReadResult Read(std::string& chunk);
  void Finish();
  void Fail();
  void Cancel();

 private:
  std::size_t capacity_;
  std::size_t buffered_ = 0;
  std::deque<std::string> chunks_;
  bool finished_ = false;
  bool failed_ = false;
  bool canceled_ = false;
  std::mutex mutex_;
  std::condition_variable readable_;
  std::condition_variable writable_;
};

}  // namespace lwweb
