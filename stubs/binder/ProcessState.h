#ifndef STUB_BINDER_PROCESSSTATE_H
#define STUB_BINDER_PROCESSSTATE_H

namespace android {
class ProcessState {
public:
  static ProcessState* self() {
    static ProcessState instance;
    return &instance;
  }

  void startThreadPool() {}
};
}

#endif
