#ifndef STUB_UTILS_STRING16_H
#define STUB_UTILS_STRING16_H

#include <string>

namespace android {
class String16 {
public:
  String16() = default;
  explicit String16(const char* value) : mValue(value ? value : "") {}
  explicit String16(const std::string& value) : mValue(value) {}

  const std::string& str() const { return mValue; }

  friend bool operator==(const String16& lhs, const String16& rhs) { return lhs.mValue == rhs.mValue; }
  friend bool operator!=(const String16& lhs, const String16& rhs) { return !(lhs == rhs); }

private:
  std::string mValue;
};
}

#endif
