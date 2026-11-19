#ifndef STUB_BINDER_ISERVICEMANAGER_H
#define STUB_BINDER_ISERVICEMANAGER_H

#include <memory>
#include <string>
#include <utility>

#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/Vector.h"

namespace android {
template <typename T>
class sp {
public:
  sp() = default;
  sp(std::nullptr_t) : mPtr(nullptr) {}
  sp(T* ptr) : mPtr(ptr) {}
  sp(const std::shared_ptr<T>& ptr) : mPtr(ptr) {}

  T* get() const { return mPtr.get(); }
  T* operator->() const { return mPtr.get(); }
  operator bool() const { return static_cast<bool>(mPtr); }
  bool operator==(std::nullptr_t) const { return mPtr == nullptr; }
  bool operator!=(std::nullptr_t) const { return mPtr != nullptr; }
  sp& operator=(T* ptr) {
    mPtr.reset(ptr);
    return *this;
  }

private:
  std::shared_ptr<T> mPtr;
};

class IBinder {
public:
  virtual ~IBinder() = default;
};

namespace binder {
class Status {
public:
  Status() : mOk(true), mMessage("OK") {}
  explicit Status(bool ok, std::string message = "OK") : mOk(ok), mMessage(std::move(message)) {}

  static Status ok() { return Status(true, "OK"); }
  bool isOk() const { return mOk; }
  String8 toString8() const { return String8(mMessage.c_str()); }

private:
  bool mOk;
  std::string mMessage;
};
}

class IServiceManager {
public:
  virtual ~IServiceManager() = default;
  virtual sp<IBinder> getService(const String16&) { return sp<IBinder>(nullptr); }
  virtual Vector<String16> listServices() { return {}; }
};

class StubServiceManager : public IServiceManager {};

inline sp<IServiceManager> defaultServiceManager() {
  static sp<IServiceManager> manager(new StubServiceManager());
  return manager;
}

template <typename T>
sp<T> interface_cast(const sp<IBinder>&) {
  return sp<T>(new T());
}
}

#endif
