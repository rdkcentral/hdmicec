#ifndef STUB_UTILS_STRING8_H
#define STUB_UTILS_STRING8_H

#include <string>

#include "String16.h"

namespace android {
class String8 {
public:
  String8() = default;
  explicit String8(const char* value) : mValue(value ? value : "") {}
  explicit String8(const String16& value) : mValue(value.str()) {}

  const char* c_str() const { return mValue.c_str(); }
  const char* string() const { return mValue.c_str(); }

private:
  std::string mValue;
};
}

#endif
