#ifndef SDCARD_LOG_H
#define SDCARD_LOG_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

class SdcardLogBuilder {
 public:
  SdcardLogBuilder() { buffer_[0] = '\0'; }

  void append(const char *value) {
    append_formatted("%s", value == nullptr ? "(null)" : value);
  }

  void append(char *value) { append(static_cast<const char *>(value)); }
  void append(char value) { append_formatted("%c", value); }
  void append(bool value) { append(value ? "true" : "false"); }

  template <typename T,
            typename std::enable_if<std::is_integral<T>::value &&
                                        !std::is_same<T, bool>::value &&
                                        !std::is_same<T, char>::value,
                                    int>::type = 0>
  void append(T value) {
    if (std::is_signed<T>::value) {
      append_formatted("%lld", static_cast<long long>(value));
    } else {
      append_formatted("%llu", static_cast<unsigned long long>(value));
    }
  }

  const char *c_str() const { return buffer_; }

 private:
  template <typename... Args>
  void append_formatted(const char *format, Args... args) {
    if (length_ >= sizeof(buffer_) - 1u) {
      return;
    }

    const int written =
        std::snprintf(buffer_ + length_, sizeof(buffer_) - length_, format, args...);
    if (written < 0) {
      return;
    }

    const size_t written_size = static_cast<size_t>(written);
    if (written_size >= sizeof(buffer_) - length_) {
      length_ = sizeof(buffer_) - 1u;
      buffer_[length_] = '\0';
      return;
    }

    length_ += written_size;
  }

  char buffer_[256];
  size_t length_ = 0u;
};

namespace Trace {
template <typename... Args>
void Debug(const Args &...args) {
  SdcardLogBuilder builder;
  (builder.append(args), ...);
  std::printf("%s\n", builder.c_str());
}

template <typename... Args>
void Log(const char *tag, const Args &...args) {
  SdcardLogBuilder builder;
  builder.append(tag);
  builder.append(": ");
  (builder.append(args), ...);
  std::printf("%s\n", builder.c_str());
}
}  // namespace Trace

#endif
