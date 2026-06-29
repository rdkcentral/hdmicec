#ifndef STUB_COM_RDK_HAL_HDMICEC_IHDMICEC_H
#define STUB_COM_RDK_HAL_HDMICEC_IHDMICEC_H

#include <string>
#include <vector>

#include "binder/IServiceManager.h"
#include "com/rdk/hal/hdmicec/IHdmiCecController.h"
#include "com/rdk/hal/hdmicec/IHdmiCecEventListener.h"

namespace com { namespace rdk { namespace hal { namespace hdmicec {
class IHdmiCec : public android::IBinder {
public:
  virtual ~IHdmiCec() = default;

  static std::string serviceName() { return "com.rdk.hal.hdmicec.IHdmiCec/default"; }

  virtual android::binder::Status open(const android::sp<IHdmiCecEventListener>&, android::sp<IHdmiCecController>* controller) {
    if (controller) {
      *controller = android::sp<IHdmiCecController>(new IHdmiCecController());
    }
    return android::binder::Status::ok();
  }

  virtual android::binder::Status close(const android::sp<IHdmiCecController>&, bool* result) {
    if (result) {
      *result = true;
    }
    return android::binder::Status::ok();
  }

  virtual android::binder::Status getLogicalAddresses(std::vector<int32_t>* addresses) {
    if (addresses) {
      addresses->clear();
    }
    return android::binder::Status::ok();
  }
};
}}}}

#endif
